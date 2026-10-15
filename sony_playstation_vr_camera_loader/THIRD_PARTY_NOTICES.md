# Third-party notices

The app's own code is MIT licensed (`LICENSE`). This file lists everything it contains or
builds on that comes from others. Inside the app image, the licence texts are in
`/app/licenses/`.

## Included in the app image

### RNNoise

* Source: <https://github.com/xiph/rnnoise>, git submodule `src/c/rnnoise`, release **v0.2**
  (commit `904a876dce1f9ab8860c0a5000ed151f9f6eef58`), unmodified.
* Trained model `rnnoise_data-0b50c45` (`src/rnnoise_data.c`, `src/rnnoise_data.h`), fetched
  at build time and checked against the official v0.2 release archive:
  * `rnnoise_data.c` SHA-256 `dece58eabca6a722a0bd9dbc8dfadba27abac1271760fad65e1071f54717dc78`
  * `rnnoise_data.h` SHA-256 `09ff880bddd0fc74a2ae0e5ec6c8d65714031b08d0c3f672493acd9e189c5855`
* Use: linked statically into `psvr-capture` for the optional noise suppression.
* Licence: BSD-3-Clause. Copyright (c) 2007-2017, 2024 Jean-Marc Valin; (c) 2023 Amazon;
  (c) 2017 Mozilla; (c) 2005-2017 Xiph.Org Foundation; (c) 2003-2004 Mark Borgerding.
  Full text: `/app/licenses/RNNoise-BSD-3-Clause.txt` (upstream `COPYING`).

### go2rtc

* Source: <https://github.com/AlexxIT/go2rtc>, release binary **v1.9.14**, downloaded at build
  time and verified by SHA-256 (see `Dockerfile`), unmodified.
* Use: MJPEG, RTSP and WebRTC streams of the camera picture.
* Licence: MIT, Copyright (c) 2022 Alexey Khit. Full text: `licenses/go2rtc-MIT.txt`.

### AK5703 register values from PSCam4Windows

* Source: <https://github.com/BumbleTree/PSCam4Windows> (`Ps4Ak5703Init.h`).
* Use: the register sequence that starts the camera's AK5703 microphone ADC, adapted in
  `src/python/psvr-services/ak5703.py`.
* Licence: GPL-2.0. `ak5703.py` is therefore licensed under the GPL-2.0, not MIT. Full text:
  `licenses/GPL-2.0.txt`.

### Alpine packages

The image is built on Home Assistant's Alpine base image (`ghcr.io/home-assistant/base`, see
`BUILD_FROM` in the `Dockerfile`) and installs Alpine packages at the versions pinned there:
`libturbojpeg` from [libjpeg-turbo](https://libjpeg-turbo.org/) (IJG, BSD-3-Clause and zlib licences, linked
dynamically by `psvr-capture`), FFmpeg, Python 3, paho-mqtt, PulseAudio and ALSA utilities.
Each package keeps its own licence, listed for every package at <https://pkgs.alpinelinux.org/>.

## Research and references (no code included)

* **PS4EYECam** by bigboss, <https://github.com/bigboss-ps3dev/PS4EYECam> (GPL-2.0): the first
  open documentation of the PS4 camera's frame format and firmware loading.
* **PSCam4Windows** by BumbleTree (see above): layout of the microphone data in the video rows.
* **psxdev** (<https://github.com/psxdev/luke_firmwares>) and **prosperodev**
  (<https://github.com/prosperodev/hdcamera>): camera firmware research; the firmware upload
  follows their work.

## Not included

The camera firmware belongs to Sony Interactive Entertainment. The app neither contains nor
downloads it; users provide the file themselves. Only SHA-256 checksums of known versions are
part of the code. "PlayStation" is a trademark of Sony Interactive Entertainment; this project
is not affiliated with or endorsed by Sony.
