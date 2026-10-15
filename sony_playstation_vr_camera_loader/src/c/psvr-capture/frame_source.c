#include "frame_source.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "log.h"

typedef struct {
    void *start;
    size_t length;
} mapped_buffer_t;

struct frame_source {
    int from_file;
    int fd;
    int width;
    int height;
    double fps;
    size_t frame_bytes;
    int streaming;
    mapped_buffer_t *buffers;
    int buffer_count;
    uint8_t *file_frame;
    uint32_t file_sequence;
};

static int xioctl(int fd, unsigned long request, void *arg)
{
    int result;
    do result = ioctl(fd, request, arg);
    while (result < 0 && errno == EINTR);
    return result;
}

static double monotonic_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int configure_device(frame_source_t *s, const frame_source_config_t *config)
{
    struct v4l2_capability cap = {0};
    struct v4l2_format fmt = {0};
    struct v4l2_streamparm parm = {0};

    if (xioctl(s->fd, VIDIOC_QUERYCAP, &cap) < 0) {
        log_error("%s is not a V4L2 device: %s", config->device, strerror(errno));
        return -1;
    }
    uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps : cap.capabilities;
    if (!(caps & V4L2_CAP_VIDEO_CAPTURE) || !(caps & V4L2_CAP_STREAMING)) {
        log_error("%s cannot stream video capture buffers", config->device);
        return -1;
    }

    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = (uint32_t)config->width;
    fmt.fmt.pix.height = (uint32_t)config->height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(s->fd, VIDIOC_S_FMT, &fmt) < 0) {
        log_error("Camera rejected %dx%d YUYV: %s", config->width, config->height, strerror(errno));
        return -1;
    }
    if ((int)fmt.fmt.pix.width != config->width || (int)fmt.fmt.pix.height != config->height ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV ||
        fmt.fmt.pix.bytesperline != (uint32_t)config->width * 2) {
        log_error("Camera offered %ux%u (stride %u) instead of raw %dx%d; is the PS4 firmware loaded?",
                  fmt.fmt.pix.width, fmt.fmt.pix.height, fmt.fmt.pix.bytesperline,
                  config->width, config->height);
        return -1;
    }
    s->frame_bytes = (size_t)config->width * 2 * config->height;

    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = (uint32_t)config->fps;
    if (xioctl(s->fd, VIDIOC_S_PARM, &parm) == 0 && parm.parm.capture.timeperframe.numerator)
        s->fps = (double)parm.parm.capture.timeperframe.denominator /
                 parm.parm.capture.timeperframe.numerator;
    else
        s->fps = config->fps;

    struct v4l2_requestbuffers req = {0};
    req.count = (uint32_t)config->buffers;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(s->fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        log_error("Could not allocate capture buffers: %s", strerror(errno));
        return -1;
    }
    s->buffers = calloc(req.count, sizeof(*s->buffers));
    if (!s->buffers) return -1;
    s->buffer_count = (int)req.count;
    for (int i = 0; i < s->buffer_count; i++) {
        struct v4l2_buffer buf = {0};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (uint32_t)i;
        if (xioctl(s->fd, VIDIOC_QUERYBUF, &buf) < 0) return -1;
        s->buffers[i].length = buf.length;
        s->buffers[i].start = mmap(NULL, buf.length, PROT_READ, MAP_SHARED, s->fd, buf.m.offset);
        if (s->buffers[i].start == MAP_FAILED) {
            s->buffers[i].start = NULL;
            log_error("Could not map capture buffer %d: %s", i, strerror(errno));
            return -1;
        }
    }
    log_info("V4L2 %s: %dx%d YUYV at %.2f fps, %d buffers (%.1f MB kernel memory)",
             config->device, config->width, config->height, s->fps, s->buffer_count,
             s->buffer_count * (double)s->frame_bytes / 1e6);
    return 0;
}

frame_source_t *frame_source_open(const frame_source_config_t *config)
{
    frame_source_t *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->from_file = config->from_file;
    s->width = config->width;
    s->height = config->height;
    s->fps = config->fps;
    s->frame_bytes = (size_t)config->width * 2 * config->height;

    if (s->from_file) {
        s->fd = strcmp(config->device, "-") ? open(config->device, O_RDONLY | O_CLOEXEC) : 0;
        s->file_frame = malloc(s->frame_bytes);
        if (s->fd < 0 || !s->file_frame) {
            log_error("Cannot read raw frames from %s", config->device);
            frame_source_close(s);
            return NULL;
        }
        return s;
    }

    s->fd = open(config->device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (s->fd < 0) {
        log_error("Cannot open %s: %s", config->device, strerror(errno));
        free(s);
        return NULL;
    }
    if (configure_device(s, config) < 0) {
        frame_source_close(s);
        return NULL;
    }
    return s;
}

int frame_source_start(frame_source_t *s)
{
    if (s->streaming) return 0;
    if (!s->from_file) {
        for (int i = 0; i < s->buffer_count; i++) {
            struct v4l2_buffer buf = {0};
            buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = (uint32_t)i;
            if (xioctl(s->fd, VIDIOC_QBUF, &buf) < 0) return -1;
        }
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(s->fd, VIDIOC_STREAMON, &type) < 0) {
            log_error("Camera refused to start streaming: %s", strerror(errno));
            return -1;
        }
    }
    s->streaming = 1;
    return 0;
}

int frame_source_stop(frame_source_t *s)
{
    if (!s->streaming) return 0;
    s->streaming = 0;
    if (s->from_file) return 0;
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    /* STREAMOFF also returns every queued buffer to user space. */
    return xioctl(s->fd, VIDIOC_STREAMOFF, &type);
}

int frame_source_streaming(const frame_source_t *s) { return s->streaming; }

static int next_file_frame(frame_source_t *s, frame_t *frame)
{
    size_t filled = 0;
    while (filled < s->frame_bytes) {
        ssize_t got = read(s->fd, s->file_frame + filled, s->frame_bytes - filled);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) return -1;
        filled += (size_t)got;
    }
    frame->data = s->file_frame;
    frame->bytes_used = s->frame_bytes;
    frame->sequence = s->file_sequence;
    frame->timestamp = s->file_sequence / s->fps;
    frame->damaged = 0;
    frame->index = -1;
    s->file_sequence++;
    return 1;
}

int frame_source_next(frame_source_t *s, frame_t *frame, int timeout_ms)
{
    if (!s->streaming) return 0;
    if (s->from_file) return next_file_frame(s, frame);

    struct pollfd pfd = {.fd = s->fd, .events = POLLIN};
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready < 0) return errno == EINTR ? 0 : -1;
    if (ready == 0) return 0;
    if (pfd.revents & (POLLERR | POLLHUP)) return -1;

    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (xioctl(s->fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN) return 0;
        log_error("Camera buffer dequeue failed: %s", strerror(errno));
        return -1;
    }
    frame->data = s->buffers[buf.index].start;
    frame->bytes_used = buf.bytesused < s->buffers[buf.index].length ? buf.bytesused
                                                                     : s->buffers[buf.index].length;
    frame->sequence = buf.sequence;
    frame->damaged = (buf.flags & V4L2_BUF_FLAG_ERROR) != 0;
    frame->index = (int)buf.index;
    if ((buf.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC &&
        (buf.timestamp.tv_sec || buf.timestamp.tv_usec))
        frame->timestamp = buf.timestamp.tv_sec + buf.timestamp.tv_usec / 1e6;
    else
        frame->timestamp = monotonic_now();
    return 1;
}

void frame_source_release(frame_source_t *s, const frame_t *frame)
{
    if (s->from_file || frame->index < 0 || !s->streaming) return;
    struct v4l2_buffer buf = {0};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = (uint32_t)frame->index;
    if (xioctl(s->fd, VIDIOC_QBUF, &buf) < 0)
        log_warning("Could not return capture buffer %d: %s", frame->index, strerror(errno));
}

double frame_source_fps(const frame_source_t *s) { return s->fps; }
size_t frame_source_frame_bytes(const frame_source_t *s) { return s->frame_bytes; }

void frame_source_close(frame_source_t *s)
{
    if (!s) return;
    frame_source_stop(s);
    if (s->buffers) {
        for (int i = 0; i < s->buffer_count; i++)
            if (s->buffers[i].start) munmap(s->buffers[i].start, s->buffers[i].length);
        free(s->buffers);
        struct v4l2_requestbuffers req = {0};
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        xioctl(s->fd, VIDIOC_REQBUFS, &req);
    }
    if (s->fd > 0) close(s->fd);
    free(s->file_frame);
    free(s);
}
