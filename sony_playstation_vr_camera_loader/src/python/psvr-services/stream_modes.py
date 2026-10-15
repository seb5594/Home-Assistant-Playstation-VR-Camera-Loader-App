"""Pick the camera stream for the configured USB profile.

The firmware lists its YUYV modes (video_nodes.capture_modes). Only the raw
modes carry the microphone rows; their width is 48 + 85 * (eye width / 32):

    PS4  3448x808  1748x408  898x200     (eyes 1280x800, 640x400, 320x192)
    PS5  5148x1088 ...                   (eyes 1920x1080, ...)

The PS5 HD camera has no microphones, so its raw rows carry no audio.
"""
from dataclasses import dataclass

PROFILES = ('High', 'Medium', 'Low')
HEADER_PIXELS = 48
TRAILER_ROWS = 8
# Used when the firmware cannot be asked (older kernels, node busy).
PS4_FALLBACK = {(3448, 808): (8, 15, 30, 60), (1748, 408): (8, 15, 30, 60, 120), (898, 200): (30, 60, 120, 240)}


@dataclass(frozen=True)
class StreamMode:
    width: int
    height: int
    fps: int
    raw: bool

    @property
    def geometry(self):
        return f'{self.width}x{self.height}'


def eye_size(width, height):
    """(width, height) of one eye in a raw mode, or None for plain pictures."""
    payload = width - HEADER_PIXELS
    if payload <= 0 or payload % 85 or height <= TRAILER_ROWS:
        return None
    return payload // 85 * 32, height - TRAILER_ROWS


def generation(modes):
    """'ps5' or 'ps4' depending on the eye resolution the firmware streams."""
    eyes = {eye_size(*size) for size in modes} - {None}
    if any(width >= 1920 for width, _height in eyes):
        return 'ps5'
    if eyes:
        return 'ps4'
    return None


def has_microphones(modes):
    return generation(modes) == 'ps4'


def choose(modes, profile, audio, preview_fps):
    """StreamMode for the USB profile; audio needs a raw mode at its highest rate."""
    modes = {size: rates for size, rates in (modes or PS4_FALLBACK).items() if rates}
    raw = sorted((size for size in modes if eye_size(*size)), key=lambda size: size[0], reverse=True)
    candidates = raw or sorted(modes, key=lambda size: size[0], reverse=True)
    if not candidates:
        raise ValueError('The camera offers no usable video mode')
    index = min(PROFILES.index(profile) if profile in PROFILES else 0, len(candidates) - 1)
    width, height = candidates[index]
    rates = modes[(width, height)]
    if audio:
        fps = max(rates)        # most rows per second: smallest audio latency
    else:
        fps = min((rate for rate in rates if rate >= preview_fps), default=max(rates))
    return StreamMode(width, height, fps, bool(eye_size(width, height)))
