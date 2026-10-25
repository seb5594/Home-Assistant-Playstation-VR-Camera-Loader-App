# Changelog

## 1.0.0-beta.5

* Dependencies: the Home Assistant base image is now `3.24-2026.10.1`; curl 8.23.0 and ffmpeg 8.1.3 follow it (the old ffmpeg no longer fits the new base).

## 1.0.0-beta.4

Versions follow SemVer from now on; this is the release previously called beta4.

* The image is built on Home Assistant's Alpine base (`ghcr.io/home-assistant/base:3.24`)
  instead of Debian bookworm, with every package pinned to an exact version and kept current
  by Renovate. The image is smaller and builds the same way in the release pipeline as on
  the Home Assistant host.
* Fixed: *Speech detected* was lit in a quiet room and the automatic gain went up to +18 dB,
  so Assist got amplified noise. The noise floor estimate started far too low and rose only
  0.6 dB per second, and every lost camera frame (replaced by silence) pulled it down again.
  The new speech detector takes the floor from the levels of the last five seconds, ignores
  lost frames and needs 8 dB over the floor for 48 ms before it reports speech.
* The automatic gain never lifts the room's noise above −50 dBFS; in a loud room it stays at
  0 dB, and it eases down when the room gets louder (a fan or TV switched on).
* *Input* is measured after the 100 Hz high-pass, so DC offset and rumble no longer read as
  sound. *Technical details* shows the noise floor.
* App page: the second level is called *Output*, the speech focus tells whether it is
  following a talker or keeping the last direction, and *Speech enhancement* links straight to
  the app configuration.
* Uses `bashio::app.options` where the base image has it (no deprecation warning in the log).
* Fixed: the aarch64 image did not build. RNNoise 0.2 compiles its NEON code there, which includes
  a header the release does not ship; the build script now supplies it.

## beta3

Microphone:

* New default beamformer `focus` against TV, radio and music. It learns where the steady
  background comes from, uses the moment a voice rises above it (usually the wake word) to
  find the talker, and then keeps the talker's direction while suppressing the background.
  In the simulated rooms of the test suite it improves the talker-to-TV ratio by about
  5–9 dB over the plain average (`adaptive`: 0–3.5 dB). `adaptive` and `sum` stay available.
* The ≈ 194 kHz capsule signal is decimated before resampling, which halves the CPU time of
  the microphone path.
* The web page shows how much of the 300–7000 Hz band the focus has locked onto a talker.

Camera:

* The picture controls are app options now (`picture_brightness`, `picture_contrast`,
  `picture_gamma`, `picture_saturation`, `picture_auto_levels`, `picture_white_balance`,
  `picture_noise_filter`). They set the start values, and changes on the app page are written
  back to the app configuration.

Project:

* RNNoise is a git submodule of the upstream repository (v0.2) instead of a downloaded copy;
  the trained model is checked against the official release.
* Sources are sorted by language and project (`src/c/psvr-capture`, `src/c/rnnoise`,
  `src/python/psvr-services`). Tests, simulations and the benchmark moved to the repository's
  `.github` folder and run on GitHub; installing the app no longer compiles and runs them,
  so installs are faster.
* MIT licence for the app's own code; third-party notices and licence texts are part of the
  image. `ak5703.py` stays GPL-2.0.
* Documentation split into a short README and the full DOCS, with system requirements per
  setting. The app remains experimental.

## beta2

* Fixed: picture settings on the app page were not saved. Home Assistant's ingress proxy
  sends request bodies chunked, without Content-Length, and the web server only read the
  latter. The firmware upload had the same problem.
* The preview arrives as one MJPEG stream instead of single pictures, so it reaches the
  configured frame rate through ingress (polling stopped at about 9 fps).
* Fixed: the microphone input sometimes did not show up in Home Assistant. If the default
  speaker no longer existed, restoring it failed and aborted the registration before Home
  Assistant was told about the new input. Hosts without any speaker now get a silent
  stand-in output, so Home Assistant's replies are not fed back into the microphone.
* Fixed: in a quiet room the parser re-emitted the whole sample window on rows it could
  not match, which duplicated samples and pushed the measured rate from about 194 kHz to
  about 244 kHz. Such rows now keep the layout's usual step.
* Picture: white balance works in dark rooms (mid-tones are taken relative to the scene),
  automatic exposure may stretch up to about 6x and lifts dark scenes with an exposure
  gamma. The colour cast of the camera is corrected more completely.
* Technical details list the camera's own V4L2 controls (exposure, gain, …).

## beta1

Microphone:

* The layout probe also recognises interleaved frames (4 capsules per frame) and shows
  "interleaved" when it finds them.
* The sample clock estimate starts from a frame parsed with the final layout. Before, it
  started from the probe frame and could be several percent off (wrong pitch) until the
  first correction three seconds later.
* Automatic gain aims at −18 dBFS and never goes below 0 dB; the limiter handles peaks.
* Echo cancellation with `default` no longer ends up on the camera's own PulseAudio
  transport. If PulseAudio switches the default output to the transport when it appears,
  the previous output is restored. Echo errors are shown translated on the page.

Picture:

* Limited-range YUV is expanded to full range: black is black instead of grey fog.
* Automatic exposure levels (at most 2x gain, so dim rooms do not turn into noise), white
  balance from near-grey surfaces only, colour, brightness, contrast, shadows and a
  motion-adaptive noise filter, all with lookup tables (about 2 ms per SD frame on x86).
* New *Picture* card on the app page: every control applies live and is kept in
  `video_tuning.json`. The preview shows a frame counter.
* The preview is always shown in mode Camera or Both; the 5 fps limit in mode Both is gone.
* Damaged frames are not shown, and the noise filter starts fresh after a pause.

Firmware and camera detection:

* The app no longer contains any camera firmware. Add it once on the app page (upload) or
  copy it into the configuration folder. Existing installations keep the files earlier
  versions copied there. Tested images are recognised by SHA-256.
* `camera_model: auto` (new default): uses the only firmware present, otherwise learns per USB
  socket which firmware gave a picture, starting with PS4 and switching after a failed start.
  In boot mode both cameras look identical on USB, so there is nothing to read beforehand.
* The stream geometry comes from the modes the running firmware offers (`VIDIOC_ENUM_FRAMESIZES`)
  instead of a fixed table. PS5 raw modes (5148x1088) are understood; the PS5 HD camera has no
  microphones, so it runs video only and the page says so.

Configuration (migrated automatically on first start):

* `output_mode` (Mikrofon/Kamera/Beides) is now `mode` with English values Microphone, Camera, Both.
* Removed `firmware_file` (a custom file is copied to the standard name), `show_video_preview`,
  `video_gamma` and `video_contrast` (now live controls; old values are not carried over because
  they compensated for the missing range expansion).
* All options and page texts translated (English and German).

Web page:

* The API answers only the Home Assistant ingress proxy and go2rtc inside the app.
* Picture settings saved from two tabs at once no longer collide.


## alpha12 – alpha15

* Adjusted default values for the camera stream.
* Replaced the options `usb_mode` and `fps` by `usb_profile` (High, Medium, Low) with preset
  resolution and frame rate.

## alpha11

Audio rebuilt around a small C capture engine (`psvr-capture`) that owns the camera.

* Direct V4L2 capture with 4 memory-mapped buffers instead of FFmpeg piping 170 MB/s into
  Python (FFmpeg also allocated up to 32 frame buffers, about 180 MB of kernel memory).
  Only the audio bytes of each row are read; CPU use for decoding drops from ~28 % of a
  core to under 1 %.
* Self-calibrating microphone layout: the first half second of rows decides whether the
  8 slots per channel are packed samples, a sliding window or repeats, and whether the 8 rows
  after the picture carry audio. The detected layout is shown on the web page.
* Dropped frames are detected through driver sequence numbers and replaced by the right
  amount of silence; damaged or short buffers still give up their complete rows. The old
  "alignment corrections" and their audio gaps are gone.
* The camera's real sample rate is measured and the resampler follows it, so latency stays
  constant over hours.
* Output is 16 kHz mono, the rate Assist uses; the PulseAudio transport runs at 16 kHz too.
* New voice chain: 100 Hz high-pass, capsule level matching, adaptive beamforming that
  notches steady noise sources (TV, music), optional echo cancellation against a Home
  Assistant audio output, RNNoise, speech-gated automatic gain and a limiter.
* Fixed: RNNoise blend mixed the dry signal 20 ms early, producing a comb filter below
  100 % strength.
* Fixed: the gain option only reached two of the four capsules (0x07/0x17); 0x08/0x18 now
  follow it too. Values above 89 % are capped at the documented +36 dB.
* New options: `usb_mode` (experimental low-bandwidth modes), `beamforming`, `auto_gain`,
  `echo_reference`. Existing configurations keep working; missing keys use the defaults.

Device detection:

* One code path through host sysfs and usbfs; PyUSB, pyudev and libusb removed.
* Capture nodes are identified with one `VIDIOC_QUERYCAP` ioctl instead of running
  `v4l2-ctl` for every scan.
* A disconnect during the firmware reboot request is recognised for all errno values the
  kernel uses (ENODEV, EPROTO, ESHUTDOWN, …), not only ENODEV.

Web page and image:

* New self-explanatory ingress page (German and English): status, live level meters,
  speech indicator, copyable input name, before/after listening test, preview.
  Removed the raw JSON dumps and the diagnostics polling that ran `v4l2-ctl` and
  `media-ctl` every three seconds. Polling stops while the page is hidden.
* Preview JPEGs are produced by the engine with TurboJPEG only while someone watches;
  FFmpeg remains only for go2rtc's on-demand H.264.
* Removed packages: python3-usb, python3-pyudev, v4l-utils, uhubctl, kmod,
  libasound2-plugins. The app no longer requests `kernel_modules`.
* AppArmor profile renamed to the app slug, as Supervisor expects.
* The disabled `reflash_on_start` port power cycling code is removed.

## alpha10

* Bootloader and initialized cameras found through host sysfs when PyUSB sees nothing;
  usbfs fallback for firmware and ADC control transfers.
* PulseAudio microphone registered only while an initialized camera is present.
* Output mode values shown in German (Mikrofon, Kamera, Beides).

## alpha9

* Event-driven camera detection (inotify on the host /dev) instead of a scan interval.
* Options grouped as Microphone and Camera; MQTT moved into Camera.

## alpha8

* Optional RNNoise noise reduction; recovery messages summarised once per minute.

## alpha7

* One canonical microphone input `sony_playstation_vr_camera_input`; Music Assistant ducking blueprint.

## alpha6 and earlier

* 15/30/60 fps microphone capture, raw `-c:v copy` capture path, frame sync fixes,
  Supervisor audio reload, MQTT camera entity, go2rtc preview.
