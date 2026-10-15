#!/usr/bin/env bash
set -Eeuo pipefail

# Runs inside the built image (release pipeline). No camera is attached, so this
# checks that everything the app starts is present and loads on this architecture.
fixture=$(mktemp -d)
trap 'rm -rf -- "$fixture"' EXIT

pass() { echo "PASS: $*"; }

# Capture engine: links against RNNoise and TurboJPEG and knows the speech focus.
help=$(psvr-capture --help)
grep -q -- '--audio' <<<"$help"
grep -q 'focus' <<<"$help"
pass 'psvr-capture starts and offers the speech focus beamformer'

# A tenth of a second of empty raw frames (profile High) through the voice chain.
head -c $((3448 * 808 * 2 * 6)) /dev/zero > "$fixture/frames.raw"
if ! psvr-capture --file --device "$fixture/frames.raw" --geometry 3448x808 --fps 60 --audio \
    --beamformer focus > "$fixture/voice.pcm" 2> "$fixture/engine.log" \
  || grep -q '\[ERROR\]' "$fixture/engine.log"; then
  cat "$fixture/engine.log"
  exit 1
fi
pass 'psvr-capture reads recorded frames without errors'

# go2rtc for the camera streams, and the tools the services call.
go2rtc -version >/dev/null
for tool in pactl parec aplay ffmpeg jq; do
  command -v "$tool" >/dev/null || { echo "FAIL: $tool is missing"; exit 1; }
done
pass 'go2rtc, PulseAudio, ALSA and FFmpeg tools are installed'

# Every Python service imports with the image's Python and paho-mqtt.
(cd /app/src && python3 -c 'import camera_loader, capture_service, web, mqtt_camera, alsa_loopback, go2rtc_config')
python3 /app/src/go2rtc_config.py "$fixture/go2rtc.yaml"
grep -q 'playstation_camera_mjpeg' "$fixture/go2rtc.yaml"
pass 'Python services import and render the go2rtc configuration'

# Start script and licence texts.
bash -n /etc/s6-overlay/s6-rc.d/sony-playstation-vr-camera/run
for licence in LICENSE THIRD_PARTY_NOTICES.md GPL-2.0.txt go2rtc-MIT.txt RNNoise-BSD-3-Clause.txt; do
  test -s "/app/licenses/$licence" || { echo "FAIL: /app/licenses/$licence is missing"; exit 1; }
done
pass 'start script parses and licence texts are part of the image'
