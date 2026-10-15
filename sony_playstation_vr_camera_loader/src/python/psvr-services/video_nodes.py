"""Locate the V4L2 capture node that belongs to a camera USB socket.

uvcvideo creates two nodes per camera (picture and metadata). Instead of
spawning v4l2-ctl for every scan, the node's capabilities are read with one
VIDIOC_QUERYCAP ioctl, which works even while another process streams.
"""
import fcntl
import os
from pathlib import Path
import struct

from runtime import SONY_VENDOR, UVC_PIDS
from usb_device import usb_path_of

VIDEO_CLASS = Path('/sys/class/video4linux')
DEV = Path('/dev')

_CAPABILITY = struct.Struct('16s32s32sIII12x')        # struct v4l2_capability, 104 bytes
_FRAME_SIZE = struct.Struct('IIIII16x8x')            # struct v4l2_frmsizeenum, 44 bytes
_FRAME_INTERVAL = struct.Struct('IIIIIII16x8x')      # struct v4l2_frmivalenum, 52 bytes
_QUERY_CONTROL = struct.Struct('II32siiiiI8x')       # struct v4l2_queryctrl, 68 bytes
_CONTROL = struct.Struct('Ii')                       # struct v4l2_control


def _ioc(direction, number, size):
    return (direction << 30) | (size << 16) | (ord('V') << 8) | number


VIDIOC_QUERYCAP = _ioc(2, 0, _CAPABILITY.size)
VIDIOC_ENUM_FRAMESIZES = _ioc(3, 74, _FRAME_SIZE.size)
VIDIOC_ENUM_FRAMEINTERVALS = _ioc(3, 75, _FRAME_INTERVAL.size)
VIDIOC_QUERYCTRL = _ioc(3, 36, _QUERY_CONTROL.size)
VIDIOC_G_CTRL = _ioc(3, 27, _CONTROL.size)
CTRL_FLAG_NEXT = 0x80000000
CTRL_FLAG_DISABLED = 0x0001
CTRL_TYPES_WITH_VALUE = {1, 2, 3}                    # integer, boolean, menu
CAP_VIDEO_CAPTURE = 0x00000001
CAP_DEVICE_CAPS = 0x80000000
PIX_FMT_YUYV = int.from_bytes(b'YUYV', 'little')
FRMSIZE_DISCRETE = 1
FRMIVAL_DISCRETE = 1


def _usb_parent(entry):
    """Walk up from /sys/class/video4linux/videoN to its USB device directory."""
    for parent in entry.resolve().parents:
        if (parent / 'idVendor').exists() and (parent / 'busnum').exists():
            return parent
    return None


def is_capture_node(node):
    try:
        fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return False
    try:
        raw = fcntl.ioctl(fd, VIDIOC_QUERYCAP, bytes(_CAPABILITY.size))
    except OSError:
        return False
    finally:
        os.close(fd)
    _driver, _card, _bus, _version, capabilities, device_caps = _CAPABILITY.unpack(raw)
    caps = device_caps if capabilities & CAP_DEVICE_CAPS else capabilities
    return bool(caps & CAP_VIDEO_CAPTURE)


def _enumerate(fd, request, layout, fields):
    """Yield the entries of a V4L2 ENUM ioctl; fields(index) fills the request."""
    for index in range(64):
        try:
            raw = fcntl.ioctl(fd, request, layout.pack(*fields(index)))
        except OSError:
            return          # EINVAL marks the end of the list
        yield layout.unpack(raw)


def capture_modes(node):
    """YUYV modes the running firmware offers: {(width, height): (fps, ...)}.

    The firmware decides these, so they tell which raw layouts (and which
    camera generation's firmware) are active without guessing from USB ids.
    """
    try:
        fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return {}
    modes = {}
    try:
        sizes = _enumerate(fd, VIDIOC_ENUM_FRAMESIZES, _FRAME_SIZE, lambda i: (i, PIX_FMT_YUYV, 0, 0, 0))
        for _index, _format, kind, width, height in list(sizes):
            if kind != FRMSIZE_DISCRETE:
                continue
            intervals = _enumerate(fd, VIDIOC_ENUM_FRAMEINTERVALS, _FRAME_INTERVAL,
                                   lambda i: (i, PIX_FMT_YUYV, width, height, 0, 0, 0))
            rates = {round(denominator / numerator)
                     for *_head, kind, numerator, denominator in intervals
                     if kind == FRMIVAL_DISCRETE and numerator}
            modes[(width, height)] = tuple(sorted(rates))
    finally:
        os.close(fd)
    return modes


def controls(node, limit=24):
    """Picture controls the firmware exposes (exposure, gain, ...), for diagnostics.

    [{'name', 'value', 'minimum', 'maximum', 'default'}]; empty if the node is gone.
    """
    try:
        fd = os.open(node, os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC)
    except OSError:
        return []
    found, control_id = [], 0
    try:
        while len(found) < limit:
            try:
                raw = fcntl.ioctl(fd, VIDIOC_QUERYCTRL, _QUERY_CONTROL.pack(control_id | CTRL_FLAG_NEXT, 0, b'',
                                                                             0, 0, 0, 0, 0))
            except OSError:
                break                                # no further controls
            control_id, kind, name, minimum, maximum, _step, default, flags = _QUERY_CONTROL.unpack(raw)
            if flags & CTRL_FLAG_DISABLED or kind not in CTRL_TYPES_WITH_VALUE:
                continue
            try:
                _id, value = _CONTROL.unpack(fcntl.ioctl(fd, VIDIOC_G_CTRL, _CONTROL.pack(control_id, 0)))
            except OSError:
                value = None
            found.append({'name': name.split(b'\0', 1)[0].decode('ascii', 'replace'), 'value': value,
                          'minimum': minimum, 'maximum': maximum, 'default': default})
    finally:
        os.close(fd)
    return found


def camera_nodes(video_class=VIDEO_CLASS, dev=DEV, probe=is_capture_node):
    """Capture nodes of initialized Sony cameras: {(bus, ports): [node, ...]}."""
    nodes = {}
    entries = sorted(Path(video_class).glob('video*'), key=lambda p: int(p.name[5:] or 0))
    for entry in entries:
        node = Path(dev) / entry.name
        usb = _usb_parent(entry)
        if usb is None or not node.exists():
            continue
        try:
            if int((usb / 'idVendor').read_text(), 16) != SONY_VENDOR:
                continue
            if int((usb / 'idProduct').read_text(), 16) not in UVC_PIDS:
                continue
            if int((entry / 'index').read_text()) != 0:
                continue  # index 1 is the metadata node
            path = usb_path_of(usb)
        except (OSError, ValueError):
            continue
        if probe(str(node)):
            nodes.setdefault(path, []).append(str(node))
    return nodes


def node_matches(node, bus, ports, product_id, video_class=VIDEO_CLASS):
    """True while `node` still belongs to the same camera (no stale status)."""
    if not Path(node).exists():
        return False
    usb = _usb_parent(Path(video_class) / Path(node).name)
    if usb is None:
        return False
    try:
        return (usb_path_of(usb) == (bus, tuple(ports)) and
                int((usb / 'idVendor').read_text(), 16) == SONY_VENDOR and
                int((usb / 'idProduct').read_text(), 16) == product_id)
    except (OSError, ValueError):
        return False
