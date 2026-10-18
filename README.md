<!-- header:begin (generated from project metadata) -->
<h1 align="center">Sony PlayStation VR Camera Loader</h1>

<p align="center">
  <img src="https://raw.githubusercontent.com/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App/main/sony_playstation_vr_camera_loader/logo.png" alt="Sony PlayStation VR Camera Loader" width="360" height="138">
</p>

---

<p align="center">
  <a href="https://paypal.me/seb5594"><img src="https://img.shields.io/static/v1?label=Support&amp;message=PayPal&amp;color=0070BA&amp;logo=paypal&amp;logoColor=white&amp;style=for-the-badge" alt="PayPal" height="40"></a>
  <a href="https://buymeacoffee.com/seb5594"><img src="https://img.shields.io/static/v1?label=Support&amp;message=Buy%20Me%20a%20Coffee&amp;color=FFDD00&amp;logo=buy-me-a-coffee&amp;logoColor=black&amp;style=for-the-badge" alt="Buy Me a Coffee" height="40"></a>
</p>

<p align="center">
  <a href="https://my.home-assistant.io/redirect/supervisor_add_addon_repository/?repository_url=https%3A%2F%2Fgithub.com%2Fseb5594%2FHome-Assistant-Apps"><img src="https://img.shields.io/static/v1?label=Home%20Assistant&amp;message=Add%20app%20repository&amp;color=18BCF2&amp;logo=homeassistant&amp;logoColor=white&amp;style=for-the-badge" alt="Home Assistant" height="28"></a>
  <a href="https://github.com/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App/releases"><img src="https://img.shields.io/static/v1?label=version&amp;message=1.0.0-beta.4&amp;color=1877A5&amp;style=for-the-badge" alt="version" height="28"></a>
</p>

<p align="center">
  <a href="https://github.com/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App/issues"><img src="https://img.shields.io/github/issues/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App?label=issues&amp;style=flat" alt="issues" height="20"></a>
  <a href="https://github.com/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App/actions/workflows/ci.yml"><img src="https://img.shields.io/github/actions/workflow/status/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App/ci.yml?branch=main&amp;label=checks&amp;style=flat" alt="checks" height="20"></a>
  <a href="https://github.com/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App#license"><img src="https://img.shields.io/github/license/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App?label=license&amp;style=flat" alt="license" height="20"></a>
</p>

<p align="center">
  <img src="https://img.shields.io/static/v1?label=arch&amp;message=amd64%20%C2%B7%20aarch64&amp;color=157F71&amp;style=flat" alt="arch" height="20">
  <img src="https://img.shields.io/static/v1?label=mounts%20%28read-write%29&amp;message=addon_config&amp;color=B45309&amp;style=flat" alt="mounts (read-write)" height="20">
  <img src="https://img.shields.io/static/v1?label=local%20disks&amp;message=attached&amp;color=1877A5&amp;style=flat" alt="local disks" height="20">
  <img src="https://img.shields.io/static/v1?label=stage&amp;message=experimental&amp;color=2F855A&amp;style=flat" alt="stage" height="20">
  <img src="https://img.shields.io/static/v1?label=ingress&amp;message=enabled&amp;color=7C3AED&amp;style=flat" alt="ingress" height="20">
  <img src="https://img.shields.io/static/v1?label=options&amp;message=26&amp;color=1877A5&amp;style=flat" alt="options" height="20">
</p>
<!-- header:end -->

A Home Assistant app that turns the PlayStation 4 / PlayStation VR camera into a far-field
microphone for Assist: four capsules, speech focus against TV and music, one clean 16 kHz
input for any voice satellite. The camera picture comes along as a bonus.

## Why

Voice control in Home Assistant lives and dies with the microphone. A satellite with one cheap
capsule on a shelf hears the TV better than the person on the sofa, and proper microphone
arrays cost more than the rest of the satellite. The PS4 camera has four capsules in a row,
15 cm wide, and sells second-hand for very little – it just never had a driver that exposes
them on Linux.

This project gives it one. The app uploads the camera firmware, starts the microphone ADC,
pulls the four channels out of the raw video stream and turns them into one clean 16 kHz
voice signal that any Assist satellite can use. The camera does not stop being a camera, so
the app also offers its picture – a nice extra on top of a good microphone, not the main act.

**The app is experimental.** The microphone format is undocumented and was reverse-engineered
on the hardware at hand. It runs well here; your camera, USB controller and room may teach it
something new.

## Features

* **Speech focus** against TV, radio and music: the four capsules learn where the steady
  background comes from, the wake word shows where the talker is, and the beam keeps the
  talker while it suppresses the background.
* Optional [RNNoise](https://github.com/xiph/rnnoise) noise suppression, echo cancellation for
  sound Home Assistant plays itself, speech-gated automatic gain and a limiter.
* One small C program does all the per-sample work, sized for low-budget hardware.
* Live preview, MQTT camera entity and go2rtc streams (MJPEG, on-demand H.264); picture
  controls on the app page are written back to the app configuration.
* PS4 / PS VR camera (CUH-ZEY1, CUH-ZEY2) with microphones; PS5 HD camera (CFI-ZEY1) as a video
  camera.

## Installation

1. Add the app catalog to Home Assistant with the *Add app repository* button above, or under
   **Settings → Apps → App store → ⋮ → Repositories** with
   `https://github.com/seb5594/Home-Assistant-Apps`.
2. Install **Sony PlayStation VR Camera Loader** and follow its *Documentation* tab.

You also need the camera firmware file, which is not part of this repository (see the app's
documentation).

## System requirements

The short version; measured numbers per setting are in
[`DOCS.md`](sony_playstation_vr_camera_loader/DOCS.md#system-requirements). x86 values were
measured on two cloud CPUs and are given as a range; Raspberry Pi values marked *est.* are
deliberately pessimistic estimates (four times the x86 range), not measurements. Percentages
are of one CPU core. Keep a generous margin: Assist, the wake word engine and the satellite
need CPU on the same machine.

* **USB 3 is mandatory in every setting.** The microphones only exist inside the raw video
  stream, and with the microphone on even the smallest profile sends about 86 MB/s. Home Assistant Green and Yellow
  (USB 2.0 ports only), Raspberry Pi 3 and older, USB 2.0 hubs and extensions cannot run it.
* **64-bit:** `amd64` or `aarch64`.

| Setting | CPU, x86 (measured) | CPU, Raspberry Pi 4 (est.) | RAM (plan for) |
| --- | --- | --- | --- |
| `Microphone`, `beamforming: sum` | 2–4 % | 8–15 % | ≈ 150 MB |
| `Microphone`, `focus` (default) | 2.5–6 % | 10–24 % | ≈ 150 MB |
| `Microphone`, `focus` + RNNoise + echo cancellation | 5–10 % | 20–40 % | ≈ 150 MB |
| `Both`: add for the preview `sd` at 10 fps, while watched | + 2.6–4 % | + 10–16 % | ≈ 250 MB in total |
| add per H.264 viewer (WebRTC/RTSP), 640×400 at 10 fps | + 3–8 % | + 12–30 % | + ≈ 100 MB |

| Hardware | Recommended use |
| --- | --- |
| Raspberry Pi 4 (4 GB recommended) | `Microphone` with `focus`, RNNoise if the room needs it; an occasional `sd` preview at ≤ 10 fps. With a USB boot SSD on the same USB 3 link, prefer `usb_profile: Medium`. |
| Raspberry Pi 5 | as Pi 4, plus `Both` with `sd`/`native` preview at ≤ 15 fps and H.264 at 640×400 |
| x86 mini PC (Intel N100 class and up) | any combination |
| HA Green, HA Yellow, Raspberry Pi 3 | not supported (no USB 3) |

## Repository layout

```
sony_playstation_vr_camera_loader/      the app (folder = slug, also the image build context)
  config.yaml, Dockerfile, run.sh         app definition, image, service start
  src/c/psvr-capture/                     capture engine (C): V4L2, microphone decoding,
                                          speech focus, voice chain, picture processing
  src/c/rnnoise/                          RNNoise, git submodule of xiph/rnnoise, pinned to v0.2
  src/c/build-rnnoise.sh                  builds RNNoise statically and adds its trained model
  src/python/psvr-services/               firmware upload, PulseAudio input, web page, MQTT
  translations/, assets/, blueprints/, examples/
  licenses/                               licence texts of third-party code in the app
.github/
  tests/c/psvr-capture/                   unit tests, room simulations, benchmark (C)
  tests/python/psvr-services/             service and web API tests (Python)
  tests/runtime.sh                        checks run inside the built image by the release pipeline
  scripts/add-rnnoise-submodule.sh        registers the RNNoise submodule at the pinned commit
  workflows/ci.yml                        shared pipeline: validation, multi-arch build, runtime
                                          test, GHCR release, README badges
  workflows/tests.yml                     unit tests, room simulations, ShellCheck
```

Tests stay out of the app folder on purpose: installing the app on a Home Assistant host
compiles only what it ships, which keeps installs short on small boards.

## Development

```sh
git clone --recursive https://github.com/seb5594/Home-Assistant-Playstation-VR-Camera-Loader-App
cd Home-Assistant-Playstation-VR-Camera-Loader-App
APP=sony_playstation_vr_camera_loader

# RNNoise (needs autoconf, automake, libtool; downloads the model once, checks its SHA-256)
sh $APP/src/c/build-rnnoise.sh "$PWD/$APP/src/c/rnnoise" /tmp/rnnoise
export CFLAGS="-O2 -I/tmp/rnnoise/include" LDFLAGS="-L/tmp/rnnoise/lib"

make -C $APP/src/c/psvr-capture                 # the engine (needs libturbojpeg)
make -C .github/tests/c/psvr-capture test       # unit tests and room simulations
make -C .github/tests/c/psvr-capture bench      # CPU numbers for the system requirements
python3 .github/tests/c/psvr-capture/end_to_end.py $APP/src/c/psvr-capture/psvr-capture   # needs numpy
python3 -m unittest discover -s .github/tests/python/psvr-services
```

The room simulations (`test_beamformer.c`) print how much each beamformer improves the
talker-to-TV ratio in a set of rooms: TV to the side, TV ahead, a stereo TV, dialogue from the
TV, a sudden loud scene, reverberation, a fan as a second source. New scenarios are welcome,
especially ones taken from real rooms.

If the repository was set up without the submodule, `.github/scripts/add-rnnoise-submodule.sh`
adds it at the pinned commit.

Releases: raise `version` in `config.yaml` and `BUILD_VERSION` in the `Dockerfile` together and
add a `CHANGELOG.md` entry. Base image and every Alpine package are pinned to exact versions;
Renovate keeps them current.

## Credits

This project stands on the work of others:

* **[PSCam4Windows](https://github.com/BumbleTree/PSCam4Windows)** by BumbleTree – AK5703
  microphone ADC register values and the audio column layout.
* **[PS4EYECam](https://github.com/bigboss-ps3dev/PS4EYECam)** by bigboss – the first open
  research into the PS4 camera's frame format and firmware loading.
* **[psxdev](https://github.com/psxdev/luke_firmwares)** and
  **[prosperodev](https://github.com/prosperodev/hdcamera)** – camera firmware research; the
  firmware upload follows their work.
* **[RNNoise](https://github.com/xiph/rnnoise)** by Jean-Marc Valin, Xiph.Org and contributors –
  noise suppression.
* **[go2rtc](https://github.com/AlexxIT/go2rtc)** by Alexey Khit – camera streaming.
* **[libjpeg-turbo](https://libjpeg-turbo.org/)** – JPEG encoding of the preview.

The camera firmware belongs to Sony Interactive Entertainment and is not distributed here.
"PlayStation" is a trademark of Sony Interactive Entertainment; this project is not affiliated
with or endorsed by Sony.

## Licence

The project's own code is released under the [MIT licence](LICENSE). Two exceptions:

* `sony_playstation_vr_camera_loader/src/python/psvr-services/ak5703.py` contains register
  values adapted from PSCam4Windows and is licensed under the GPL-2.0
  ([text](sony_playstation_vr_camera_loader/licenses/GPL-2.0.txt)).
* RNNoise, included as a submodule, keeps its BSD-3-Clause licence.

Details for every third-party component are in
[`THIRD_PARTY_NOTICES.md`](sony_playstation_vr_camera_loader/THIRD_PARTY_NOTICES.md).

<!-- support:begin (generated, shared text) -->
## Say thanks

All my projects are built in my spare time, just for the fun of it. Still, they take a lot of time and care, and many of the smart home gadgets I buy end up here because I want to make them cloud-free and smart-home ready. Every bit of support means a lot to me.

If my work helped you or turned out to be useful, show your appreciation with a hot drink for my next sleepless night.

[![PayPal](https://img.shields.io/static/v1?label=Support&message=PayPal&color=0070BA&logo=paypal&logoColor=white&style=for-the-badge)](https://paypal.me/seb5594) [![Buy Me a Coffee](https://img.shields.io/static/v1?label=Support&message=Buy%20Me%20a%20Coffee&color=FFDD00&logo=buy-me-a-coffee&logoColor=black&style=for-the-badge)](https://buymeacoffee.com/seb5594)
<!-- support:end -->

<!-- license:begin (generated from the license file) -->
## License

[![license](https://img.shields.io/static/v1?label=license&message=MIT&color=5B6770&style=flat)](LICENSE)

Published under the [MIT License](LICENSE).
<!-- license:end -->
