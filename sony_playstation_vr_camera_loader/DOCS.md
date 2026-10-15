# Sony PlayStation VR Camera Loader

Turns a PlayStation 4 / PlayStation VR camera (CUH-ZEY1, CUH-ZEY2) into a far-field
microphone for Home Assistant Assist. The camera's four-capsule array is good at picking up
voices across a room; this app loads the camera firmware, decodes the microphones from the
raw video stream, cleans up the speech and offers it as a normal audio input. The camera
itself comes along as a bonus: a live preview, an MQTT camera entity and go2rtc streams.

The PS5 HD camera (CFI-ZEY1) works as a video camera. It has no microphones.

**This app is experimental.** It works on the hardware it was developed with, but the
camera's microphone format is not documented, and every room is different.

## Quick start

1. Plug the camera into a **USB 3** port (the PS4 camera needs an adapter cable from its AUX
   plug to USB 3). Check the [system requirements](#system-requirements) first.
2. Install and start the app, then open its web page (**Open Web UI**).
3. The first time, the page asks for the **camera firmware** (see below). After that it is
   uploaded automatically whenever the camera appears.
4. When the microphone shows **Active**, copy the input name. In your Assist satellite app
   (for example *Linux Voice Assistant* or *Wyoming Satellite*) select
   `sony_playstation_vr_camera_input` as the audio input.
5. Use **Listening test** on the web page to hear the microphone before and after processing.

## System requirements

All figures below are on the cautious side. CPU numbers were measured with `make bench` and
FFmpeg (see [Development](#development)) on two different x86 cloud cores (Intel Xeon, 2.1 and
2.8 GHz); the same code needed up to twice as long on one of them, so the tables show both
ends. Values for Raspberry Pi boards are **estimates**, not measurements: the x86 range times
2 for a Pi 5 and times 4 for a Pi 4, which is deliberately pessimistic. Percentages are of one
CPU core (a Pi 4 or Pi 5 has four).

Watch the CPU and memory graphs on the app's *Info* page after changing settings, and keep a
generous margin: Assist, the wake word engine and the satellite itself need CPU too.

### USB: a real USB 3 port is mandatory

The camera sends uncompressed frames, and the microphones only exist inside them. With the
microphone on, even the smallest profile needs more than USB 2.0 can carry. In mode `Camera`
the app picks a lower frame rate, but plan for USB 3 there too: the camera is a USB 3 device,
and running it on a USB 2.0 port has not been tested.

| `usb_profile` | Raw stream | USB data rate | V4L2 buffers in RAM |
| --- | --- | --- | --- |
| `High` (default, proven) | 3448×808 @ 60 fps | ≈ 334 MB/s (2.7 Gbit/s) | ≈ 22 MB |
| `Medium` (experimental) | 1748×408 @ 120 fps | ≈ 171 MB/s | ≈ 6 MB |
| `Low` (experimental) | 898×200 @ 240 fps | ≈ 86 MB/s | ≈ 1.5 MB |

USB 2.0 manages roughly 40 MB/s in practice. That rules out:

* **Home Assistant Green** and **Home Assistant Yellow** (USB 2.0 ports only),
* **Raspberry Pi 3** and older,
* USB 2.0 hubs, extension cables and front panel ports that are only USB 2.0.

Suitable in principle: Raspberry Pi 4 and 5 (the blue USB 3 ports), ODROID-N2+, and x86 mini
PCs. Prefer a port directly on the board, avoid passive extensions and unpowered hubs, and use
the board's official power supply; the camera is bus powered. In a virtual machine, pass the
whole USB 3 controller through or make sure the USB filter matches both `05a9:0580` (boot
loader) and `05a9:058a` to `05a9:058c` (after the firmware upload): the camera re-enumerates
once it has its firmware.

Profile `High` uses most of what one USB 3 controller manages in practice. On a Raspberry Pi 4
and an ODROID-N2+ all USB 3 ports share a single 5 Gbit/s link, so a USB boot SSD on the same
board competes with the camera; there, `Medium` is the safer choice. The CPU numbers below
do not include the kernel's own USB work for these data rates. If *Technical details* shows
dropped frames, try `Medium` and check the measured sample rate.

### CPU

Architectures: `amd64` and `aarch64`. 32-bit ARM is not supported.

**Microphone** (runs all the time). Profile `High`; the other profiles deliver about as many
rows per second, so they cost about the same CPU and mainly save USB bandwidth.

| Microphone settings | x86 core (measured) | Raspberry Pi 5 (est.) | Raspberry Pi 4 (est.) |
| --- | --- | --- | --- |
| `beamforming: sum` | 2–4 % | 4–8 % | 8–15 % |
| `adaptive` or `focus` | 2.5–6 % | 5–12 % | 10–24 % |
| `focus` + RNNoise | 4–8.5 % | 8–17 % | 16–34 % |
| `focus` + RNNoise + echo cancellation | 5–10 % | 10–20 % | 20–40 % |

**Picture** (only while someone watches: web page, MQTT camera or a go2rtc viewer). The cost
grows with `video_fps`; 30 fps costs three times the 10 fps value.

| `video_resolution` | x86, per picture | at 10 fps: x86 | Pi 5 (est.) | Pi 4 (est.) |
| --- | --- | --- | --- | --- |
| `low` (320×200) | 2–2.6 ms | 2–2.6 % | 4–5 % | 8–10 % |
| `sd` (640×400, default) | 2.6–4 ms | 2.6–4 % | 5–8 % | 10–16 % |
| `hd` (1280×720) | 5.2–6.8 ms | 5–7 % | 10–14 % | 20–28 % |
| `native` (1280×800) | 5.3–7.1 ms | 5–7 % | 10–14 % | 20–28 % |

**H.264 for WebRTC and RTSP viewers.** go2rtc starts FFmpeg (libx264, `ultrafast`) only while
such a viewer is connected; MJPEG viewers and the MQTT camera cost nothing extra. Measured on
calm and on very noisy pictures:

| Picture at 10 fps | x86 core (measured) | Raspberry Pi 5 (est.) | Raspberry Pi 4 (est.) |
| --- | --- | --- | --- |
| 640×400 (`sd`) | 3–8 % | 6–16 % | 12–30 % |
| 1280×800 (`native`) | 8–26 % | 16–50 % | 30 % to more than one core |

### Memory

| Mode | Plan for |
| --- | --- |
| `Microphone` | ≈ 150 MB (services ≈ 80 MB, capture engine ≈ 15 MB, V4L2 buffers up to 22 MB) |
| `Camera` / `Both` | ≈ 250 MB (adds go2rtc and the picture buffers) |
| each H.264 viewer | + ≈ 100 MB for FFmpeg |

### Recommended combinations

| Hardware | Good fit | Better avoid |
| --- | --- | --- |
| Raspberry Pi 4 (4 GB recommended) | `Microphone` with `focus`, RNNoise if the room needs it; in `Both`, an occasional look at `sd` or `low` at ≤ 10 fps | H.264 viewers at 1280×800, `hd`/`native` above 10 fps, echo cancellation and RNNoise together while the same board runs a local wake word and speech-to-text |
| Raspberry Pi 5 | everything above; `Both` with `sd`/`native` at ≤ 15 fps, H.264 at 640×400 | long-running H.264 viewers at 1280×800 next to a busy Assist pipeline |
| x86 mini PC (Intel N100 class and up) | any combination | – |
| HA Green, HA Yellow, Raspberry Pi 3 | – | not supported (no USB 3) |

If the board also runs Home Assistant's speech-to-text locally (for example Whisper), that
needs far more CPU than this app; plan the satellite's microphone settings around it.

## Camera firmware

Both cameras start as an OmniVision OV580 boot loader (USB id `05a9:0580`) and need their
firmware loaded into RAM every time they are plugged in. The firmware belongs to Sony and is
**not part of this app**. Community projects that use these cameras on PCs publish it; choose
the `.bin` file on the app page, or copy it into the app's configuration folder
(`/addon_configs/<id>_sony_playstation_vr_camera_loader/`) as `firmware_ps4.bin` or
`firmware_ps5.bin`. Nothing is downloaded by the app.

Files the app was tested with are recognised by their SHA-256 checksum (only the checksums
are part of the app):

| Camera | SHA-256 |
| --- | --- |
| PS4 / PS VR | `09513d1068b9b5e23b09e03bca5640ed4d01830b034b9802395006b14a927b01` |
| PS5 HD | `10af1aee76fe0057a88db7ebf5f3ebf32430633effb93722be4cd0a9ed4fce54` |

Other versions are accepted after a basic check and marked *unknown version*.

### Which camera is connected?

In boot mode both cameras report exactly the same USB descriptors, and even the product id
after the upload comes from the firmware, so neither `lsusb` nor the USB topology can tell them
apart beforehand. With `camera_model: auto` the app therefore:

1. uses the only firmware present, if there is just one;
2. remembers per USB socket which firmware gave a working picture (`camera_models.json` in the
   configuration folder);
3. tries PS4 first for an unknown camera and switches to PS5 if the camera does not come back
   or never delivers a frame.

After the upload, the video modes the firmware offers (1280x800 eyes for PS4, 1920x1080 for
PS5) decide the stream geometry, so there is no model table to keep in sync.

## How the audio is extracted

The camera has no USB audio interface. Its AK5703 converter writes the microphone samples
into a 64-byte column right after the 32-byte header of every raw video row, so audio only exists while
video streams. Everything below runs in one small C program, `psvr-capture`, which is
the only process that opens the camera:

| Step | What happens | Why |
| --- | --- | --- |
| V4L2 capture | memory-mapped driver buffers, 4 of them | earlier releases piped 170 MB/s through FFmpeg and let it allocate up to 32 buffers (≈180 MB kernel memory) |
| Row parser | reads only the 96 header/audio bytes of each row | the picture is never copied unless a preview is requested |
| Layout probe | measures the first 12,000 rows (about a quarter of a second) and decides how the 32 values are ordered (capsule by capsule or interleaved frames) and how many are new samples (packed, sliding window or repeated) | the slot layout differs per USB mode and is not documented anywhere; a misread window duplicates or smears samples |
| Gap handling | missing frames (driver sequence numbers) are replaced by the right amount of silence | keeps the audio clock and lip sync intact |
| Clock tracking | the real sample rate is measured against the host clock and the resampler follows it | the camera crystal is not the host crystal; uncorrected drift adds latency or drops |
| Decimation | a short low-pass filter keeps every 4th sample of the ≈ 194 kHz capsule signal | halves the CPU time of the whole microphone path |
| 16 kHz output | band-limited resampling straight to the rate every wake word and STT engine uses | no extra resampling inside PulseAudio |

Then the voice chain, one 16 ms block at a time:

1. **High-pass filter** at 100 Hz removes rumble, hum and footsteps.
2. **Capsule matching** keeps all four capsules at the same level and mutes a dead one.
3. **Beamforming**, see below.
4. **Echo cancellation** (optional): if Home Assistant itself plays music or voice replies on a
   known output, that output's monitor is used as a reference and its echo is subtracted.
5. **RNNoise** (optional) removes background noise. The dry part of the blend is delayed by
   RNNoise's 20 ms latency, which avoids a comb filter.
6. **Automatic gain** raises quiet, distant speech towards −18 dBFS. It only rises while
   someone speaks, never attenuates below the input level, and never lifts the room's noise
   floor above −50 dBFS: in a room that is already loud it stays at 0 dB. Speech is told apart
   from the room by its level over the noise floor of the last five seconds (with RNNoise, its
   voice detection has to agree); the page shows this as *Speech detected*.
7. **Limiter** prevents digital clipping.

### Beamforming: TV, radio and music in the room

`beamforming` chooses how the four capsules become one voice:

* **`focus`** (default, experimental) uses the moment a voice rises above the room's steady
  sound – usually the wake word – to find out where the talker is:
  1. It learns the *persistent* sound field: where the TV, the radio or a fan sends its sound
     from, per frequency.
  2. Energy that those directions cannot explain marks speech. A louder TV scene or dialogue
     from the TV stays in the TV's direction and does not count as a talker.
  3. While someone speaks over the background, a second picture of the sound field shows where
     they are. The beam keeps that direction intact (MVDR) and suppresses everything the
     background describes.
  4. The talker direction is kept after the command, so the next wake word starts focused.
     Something that "speaks" for more than six seconds without a pause, such as a vacuum
     cleaner, is moved into the background instead.
* **`adaptive`** only does step 1 and places a notch on the persistent sources. It assumes the
  talker is roughly in front of the camera and protects that direction.
* **`sum`** averages the capsules: no learning, lowest CPU.

In the simulated rooms of the test suite (`.github/tests/c/psvr-capture/test_beamformer.c`,
free field and with reverberation), `focus` improves the talker-to-TV ratio of a command by
about 5–9 dB over the plain average, where `adaptive` reaches 0–3.5 dB. A talker standing
directly between the camera and the TV cannot be separated by any beamformer; there, focus
behaves like adaptive. Real rooms reflect more than the simulation, so expect less.

### What it can and cannot do

* A steady TV or music source to the side of the camera is reduced well above about 1 kHz.
  Below that, a 15 cm array cannot tell directions apart, and RNNoise has to help.
* A TV directly behind you, or a soundbar in line with you, is in the same direction as you.
  Beamforming cannot separate that; echo cancellation can, but only for audio that Home
  Assistant plays itself.
* Another person talking in the room is speech too. `focus` follows whoever spoke over the
  background last.

## Configuration

| Option | Default | Notes |
| --- | --- | --- |
| `mode` | `Microphone` | `Microphone` runs no video. `Camera` streams pictures only while someone watches. `Both` runs microphone and picture together. |
| `camera_model` | `auto` | `ps4` or `ps5` forces that firmware. |
| `log_level` | `info` | `debug` adds per-session details. |

### Microphone

| Option | Default | Notes |
| --- | --- | --- |
| `usb_profile` | `High` | Picks one of the raw modes the firmware offers: `High` 1280x800 per eye (60 fps), `Medium` 640x400 (120 fps), `Low` 320x192 (240 fps). These are the highest frame rates, used while the microphone runs; in mode `Camera` the lowest rate that covers `video_fps` is used. Medium and Low need far less USB bandwidth but are **experimental**; check *Measured sample rate* after switching. |
| `gain` | `80` | Capsule volume, applied to all four capsules. Lower it if the page reports clipping. |
| `beamforming` | `focus` | `focus`, `adaptive` or `sum`, see above. |
| `noise_suppression` | `off` | `rnnoise` for noisy rooms. Compare wake word behaviour with the listening test. |
| `noise_strength` | `75` | Mix of filtered and original signal. |
| `auto_gain` | `true` | Speech-gated automatic gain, 0 to +18 dB, limited by the room's noise. |
| `echo_reference` | empty | Name of the audio output Home Assistant plays on (`ha audio info`), or `default`. Empty disables echo cancellation. |
| `set_default_input` | `false` | Makes the camera the shared PulseAudio default input while it is connected. |

Keep the satellite's own noise suppression and auto gain low or off while testing, so the
signal is not processed twice.

### Camera

| Option | Default | Notes |
| --- | --- | --- |
| `publish_camera_entity` | `false` | Camera entity through MQTT discovery. |
| `video_resolution` | `sd` | `native` (one full eye), `hd` (16:9 crop), `sd` (half), `low` (quarter). |
| `video_fps` | `10` | Upper limit for preview pictures per second. |
| `picture_brightness` | `0` | −100 … 100. |
| `picture_contrast` | `100` | 50 … 200 %. |
| `picture_gamma` | `100` | 50 … 250 %; above 100 lifts the shadows. |
| `picture_saturation` | `160` | 0 … 400 %; the camera delivers pale colours, especially in dim light. |
| `picture_auto_levels` | `true` | Automatic exposure levels and a shadow lift for dark scenes. |
| `picture_white_balance` | `true` | Removes the colour cast, judged from near-grey surfaces. |
| `picture_noise_filter` | `light` | `off`, `light` or `strong`. |
| `mqtt.auto_detect` | `true` | Uses the MQTT broker registered in Home Assistant (for example the Mosquitto app). |
| `mqtt.host`, `mqtt.port`, `mqtt.username`, `mqtt.password` | empty, `1883` | Broker for the camera entity when `auto_detect` is off. |

The `picture_*` options are the starting point at every app start (the last values set on the
app page are also kept in `video_tuning.json` in the configuration folder). The same controls are on
the app page (card *Picture*): changes there apply within a second and are **written back to
the app configuration**, so the Configuration tab always shows the values in use. In
installations from before beta3, the app adds the options with the current values the first
time it starts in mode `Camera` or `Both`.

In mode **Camera** or **Both** the web page always shows the preview, with a frame counter.
Preview images are made by `psvr-capture` itself (TurboJPEG) and only while someone looks: the
web page, the MQTT entity or a go2rtc viewer. In camera-only mode the USB stream is paused when
nobody has watched for about 30 seconds.

### Picture processing

The camera sends limited-range YUV (black at 16, white at 235) and has no image processor
that a webcam would have. `psvr-capture` does that part, on a quarter of the pixels where it
can and with lookup tables everywhere else:

* full-range expansion, so black is black instead of grey "fog";
* automatic exposure levels from the scene histogram, plus a shadow lift for dark scenes;
* white balance from near-grey surfaces only, so a blue wall stays blue;
* colour (saturation), brightness, contrast and shadows (gamma);
* a motion-adaptive noise filter that calms sensor noise without smearing movement.

Dim rooms suffer most: the camera's own exposure leaves them in a few dozen brightness steps
and mutes the colour, so expect some grain at night. *Technical details* lists the camera's
own controls, which may allow real exposure changes in a later version.

go2rtc offers `playstation_camera_mjpeg` and an on-demand H.264 stream `playstation_camera`.
Map ports 1984, 8554 or 8555 in the app's network settings for external viewers; the go2rtc
API has no authentication, so keep those ports on a trusted network.

## Troubleshooting

* **Camera not found**: it needs USB 3. Unplug and replug once; the log should show
  *Uploading firmware_ps4.bin* and then *running firmware as 05a9:058a*.
* **"Waiting for its firmware"**: no firmware file is stored yet, see *Camera firmware*.
* **Wrong firmware picked** (`auto` with both files present): unplug the camera for a moment;
  the app tries the other firmware next. Or set `camera_model` explicitly.
* **"No sound from the capsules"**: the ADC did not start. Replug the camera. If it persists,
  use *Download raw microphone rows* in Technical details and attach the file to an issue.
* **Dull, robotic or choppy audio**: look at *Microphone layout* and *Measured sample rate*.
  The rate must stay steady. The first camera measured in profile High sends a window that
  moves on by four samples per row, about 194 kHz; a rate that keeps changing means the slot
  layout was misdetected, and the raw rows download shows the real layout.
* **The speech focus follows the wrong source**: switch `beamforming` to `adaptive` and report
  the room setup (where camera, TV and talker are). The page shows how much of the band is
  focused on a talker.
* **Clipping warning**: lower `gain`. With `auto_gain` on, a lower capsule gain costs nothing.
* **Input level high although the room is quiet**: *Input* shows the capsules after the 100 Hz
  high-pass, so it is the real noise of camera and room; *Technical details* also shows the
  floor the automatic gain works with. If a quiet room reads higher than about −40 dBFS, lower
  `gain`. The capsule volume is digital, so this loses nothing, and loud speech close to the
  camera gets more headroom.
* **Echo cancellation off – "this app's own microphone transport"**: the entered output is the
  camera's internal PulseAudio sink. Enter the speaker output instead, or `default`; `default`
  skips the transport and uses the first real speaker.
* **Picture settings "not written to the app configuration"**: the Supervisor refused the
  update. The settings still apply until the app restarts; check the app log.

## Development

Source layout, build and test instructions are in the repository's `README.md`. The tests
live in the repository's `.github` folder and run on GitHub, so installing the app on a Home
Assistant host compiles only what it ships.

`psvr-capture --file --device recording.raw --geometry 3448x808 --fps 60 --audio > out.pcm`
processes recorded raw frames offline; give the geometry and frame rate of the recording.

## Credits and licences

The app's own code is MIT licensed (see `LICENSE`), except `ak5703.py`, whose register table is
adapted from PSCam4Windows and therefore stays under GPL-2.0. Third-party components and the
projects this work builds on are listed in `THIRD_PARTY_NOTICES.md`.
