# Sony PlayStation VR Camera Loader

![Supports aarch64](https://img.shields.io/badge/aarch64-yes-green.svg)
![Supports amd64](https://img.shields.io/badge/amd64-yes-green.svg)
![Stage: experimental](https://img.shields.io/badge/stage-experimental-orange.svg)

A far-field microphone for Assist, made from a PlayStation 4 / PlayStation VR camera.

Good microphone arrays for voice satellites are expensive. The PS4 camera has four capsules in
a row, costs little second-hand and hears voices across a living room. This app wakes it up
(firmware upload, microphone ADC), decodes the four channels from the raw video stream,
focuses on the person speaking and hands Home Assistant a clean 16 kHz input for any Assist
satellite. Its stereo camera comes along as a bonus: a live preview, an optional MQTT camera
entity and go2rtc streams.

* Speech focus: learns where the TV, the radio or the music comes from and uses the wake word
  to find the talker, then keeps that direction and suppresses the steady background.
* Optional RNNoise noise suppression, echo cancellation for Home Assistant's own playback,
  speech-gated automatic gain.
* Picture controls on the app page, written back to the app configuration.
* PS4 and PS5 camera detection; the PS5 camera works as a video camera only (no microphones).

> **Experimental.** The camera's microphone format is undocumented and was worked out on the
> hardware at hand. Expect rough edges, and please report what you see.

## Requirements

* A real **USB 3** port, in every setting. Home Assistant Green and Yellow, Raspberry Pi 3 and
  USB 2.0 hubs cannot carry the camera's data rate.
* `amd64` or `aarch64`. On a Raspberry Pi 4 the microphone with speech focus needs an
  estimated 10–25 % of one CPU core, up to about a third with RNNoise; a Raspberry Pi 5 or an
  x86 mini PC also handles the preview comfortably.
* About 150 MB of RAM for `Microphone`, about 250 MB for `Camera` or `Both`.
* The camera firmware file (not included).

The *Documentation* tab lists measured CPU numbers per setting, setup and troubleshooting.
