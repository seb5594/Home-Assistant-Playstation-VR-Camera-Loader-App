"""Create local go2rtc configuration without letting it capture USB a second time."""
import argparse
from pathlib import Path


def render():
    # MJPEG comes from our single V4L2 owner; H.264 is encoded only for WebRTC viewers.
    return '''api:
  listen: "0.0.0.0:1984"
rtsp:
  listen: "0.0.0.0:8554"
webrtc:
  listen: ":8555"
streams:
  playstation_camera_mjpeg: http://127.0.0.1:8099/preview.mjpg
  playstation_camera: ffmpeg:playstation_camera_mjpeg#video=h264
ffmpeg:
  h264: "-codec:v libx264 -threads:v 1 -g:v 5 -keyint_min 5 -sc_threshold 0 -bf 0 -preset:v ultrafast -tune:v zerolatency -pix_fmt yuv420p"
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('path', type=Path)
    args = parser.parse_args()
    args.path.parent.mkdir(parents=True, exist_ok=True)
    args.path.write_text(render(), encoding='utf-8')


if __name__ == '__main__':
    main()
