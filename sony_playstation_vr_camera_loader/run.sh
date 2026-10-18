#!/usr/bin/with-contenv bashio
# shellcheck shell=bash
# Starts the camera services. If one of the essential services stops, the
# whole app exits so the Supervisor restarts it in a clean state.

# Bashio debug output would print Supervisor replies, including MQTT passwords.
bashio::log.level info
LOG_LEVEL="$(bashio::config 'log_level')"
export LOG_LEVEL COLOR_LOGS=true
bashio::log.info 'Starting Sony PlayStation VR Camera Loader'

# ---- configuration left behind by older releases -------------------------
#
# The Supervisor drops option keys it no longer knows from options.json, so
# old values are read from the saved options and rewritten once:
#   alpha8   top-level mqtt            -> camera.mqtt
#   alpha10  output_mode Mikrofon/...  -> mode Microphone/Camera/Both
#   alpha15  firmware_file, camera_model ps4 (old default) -> camera_model auto
#   alpha15  show_video_preview, video_gamma, video_contrast -> app page controls
read -r -d '' MIGRATION <<'JQ' || true
def mode_name: {"mikrofon": "Microphone", "microphone": "Microphone", "kamera": "Camera",
                "camera": "Camera", "beides": "Both", "both": "Both"}[ascii_downcase];
if has("mqtt") then .camera = ((.camera // {}) + {mqtt: .mqtt}) | del(.mqtt) else . end
| if has("output_mode") then
    (if (.output_mode | mode_name) then .mode = (.output_mode | mode_name) else . end) | del(.output_mode)
  else . end
| if has("firmware_file") then
    (if .camera_model == "ps4" then .camera_model = "auto" else . end) | del(.firmware_file)
  else . end
| del(.scan_interval, .reflash_on_start)
| if .camera then .camera |= del(.show_video_preview, .video_gamma, .video_contrast) else . end
| if .microphone then .microphone |= del(.usb_mode, .fps) else . end
JQ

# Saved options as the user stored them. bashio renamed addon.* to app.*;
# the old name still works on older base images but logs a deprecation warning.
saved_app_options() {
  if declare -F bashio::app.options >/dev/null; then
    bashio::app.options
  else
    bashio::addon.options
  fi
}

saved_options="$(saved_app_options 2>/dev/null || true)"
if ! jq -e 'type == "object"' >/dev/null 2>&1 <<<"$saved_options"; then
  saved_options="$(cat /data/options.json)"   # Supervisor API unreachable: use what it validated
fi
OPTIONS="$(jq -c "$MIGRATION" <<<"$saved_options")"
if [[ "$OPTIONS" != "$(jq -c . <<<"$saved_options")" ]]; then
  old_firmware="$(jq -r '.firmware_file // empty' <<<"$saved_options")"
  if [[ -n "$old_firmware" ]]; then
    python3 -u /app/src/firmware_manager.py --adopt "$old_firmware" \
      --model "$(jq -r '.camera_model // "ps4"' <<<"$saved_options")" || true
  fi
  if bashio::api.supervisor POST /addons/self/options "$(jq -c '{options: .}' <<<"$OPTIONS")" >/dev/null; then
    bashio::log.info 'Configuration moved to the current option names.'
  else
    bashio::log.warning 'Could not save the updated configuration; using it for this start only.'
  fi
fi

# option <key> <default>: configured value (dotted path), or the default for
# keys that older configurations do not have yet (nested options are not merged).
option() {
  local value
  value="$(jq -r --arg path "$1" 'getpath($path | split(".")) | if . == null then empty else tostring end' \
    <<<"$OPTIONS")"
  printf '%s' "${value:-$2}"
}

case "$(option mode Microphone)" in
  Microphone) mode=microphone ;;
  Camera) mode=camera ;;
  Both) mode=both ;;
  *) bashio::log.error "Unknown mode: $(option mode '')"; exit 1 ;;
esac
camera_model="$(option camera_model auto)"

if [[ ! -e /config/alsa-sony-playstation-vr-camera.conf ]]; then
  cp /app/assets/alsa-playstation-camera.conf /config/alsa-sony-playstation-vr-camera.conf
fi

# ---- what runs ------------------------------------------------------------

video=false
entity="$(option camera.publish_camera_entity false)"
if [[ "$mode" == 'microphone' ]]; then
  if [[ "$entity" == 'true' ]]; then
    bashio::log.warning 'The MQTT camera needs mode Camera or Both; it stays off.'
    entity=false
  fi
else
  video=true
fi
export OUTPUT_MODE="$mode" PUBLISH_CAMERA_ENTITY="$entity"
VIDEO_FPS="$(option camera.video_fps 10)"
export VIDEO_FPS

capture_args=(--mode "$mode"
  --camera-model "$camera_model"
  --usb-profile "$(option microphone.usb_profile High)"
  --gain "$(option microphone.gain 80)"
  --beamforming "$(option microphone.beamforming focus)"
  --noise-suppression "$(option microphone.noise_suppression off)"
  --noise-strength "$(option microphone.noise_strength 75)"
  --echo-reference "$(option microphone.echo_reference '')")
if [[ "$(option microphone.auto_gain true)" == 'true' ]]; then
  capture_args+=(--auto-gain)
fi
if [[ "$(option microphone.set_default_input false)" == 'true' ]]; then
  capture_args+=(--set-default-input)
fi
if [[ "$video" == 'true' ]]; then
  capture_args+=(--preview "$(option camera.video_resolution sd)" --preview-fps "$VIDEO_FPS")
fi

essential=()
helpers=()

python3 -u /app/src/web.py &
essential+=($!)
python3 -u /app/src/camera_loader.py --model "$camera_model" &
essential+=($!)
python3 -u /app/src/capture_service.py "${capture_args[@]}" &
essential+=($!)

if [[ "$mode" != 'camera' && -d /sys/module/snd_aloop ]]; then
  python3 -u /app/src/alsa_loopback.py &
  helpers+=($!)
fi

if [[ "$video" == 'true' ]]; then
  python3 /app/src/go2rtc_config.py /run/ps-camera/go2rtc.yaml
  go2rtc -config /run/ps-camera/go2rtc.yaml &
  essential+=($!)
  bashio::log.info 'go2rtc started (MJPEG, and H.264 on demand for WebRTC/RTSP clients).'
fi

# ---- optional MQTT camera entity ------------------------------------------

if [[ "$entity" == 'true' || -e /config/mqtt_discovery_active ]]; then
  if [[ "$(option camera.mqtt.auto_detect true)" == 'true' ]]; then
    if bashio::services.available 'mqtt'; then
      MQTT_HOST="$(bashio::services 'mqtt' 'host')"
      MQTT_PORT="$(bashio::services 'mqtt' 'port')"
      MQTT_USERNAME="$(bashio::services 'mqtt' 'username')"
      MQTT_PASSWORD="$(bashio::services 'mqtt' 'password')"
      export MQTT_HOST MQTT_PORT MQTT_USERNAME MQTT_PASSWORD
    fi
  else
    MQTT_HOST="$(option camera.mqtt.host '')"
    MQTT_PORT="$(option camera.mqtt.port 1883)"
    MQTT_USERNAME="$(option camera.mqtt.username '')"
    MQTT_PASSWORD="$(option camera.mqtt.password '')"
    export MQTT_HOST MQTT_PORT MQTT_USERNAME MQTT_PASSWORD
  fi
  if [[ -z "${MQTT_HOST:-}" ]]; then
    [[ "$entity" == 'true' ]] && bashio::log.warning 'MQTT camera: no broker found. Install an MQTT broker app or enter one manually.'
  elif [[ "$entity" == 'true' ]]; then
    python3 -u /app/src/mqtt_camera.py &
    essential+=($!)
  else
    python3 -u /app/src/mqtt_camera.py --remove &
    helpers+=($!)
  fi
fi

trap 'kill "${essential[@]}" "${helpers[@]}" 2>/dev/null || true' TERM INT
wait -n "${essential[@]}" || true
kill "${essential[@]}" "${helpers[@]}" 2>/dev/null || true
wait || true
exit 1
