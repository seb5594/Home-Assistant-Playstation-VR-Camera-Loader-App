# SPDX-License-Identifier: GPL-2.0-only
"""Start the camera's AK5703 microphone ADC through OV580 vendor requests.

Register values are adapted from PSCam4Windows/Ps4Ak5703Init.h
(https://github.com/BumbleTree/PSCam4Windows, GPL-2.0). Unlike the rest of
the app, this file is therefore licensed under the GPL-2.0; see
licenses/GPL-2.0.txt. Without these writes the microphone columns of every
video row stay silent.

Volume: 0x07/0x08 (ADC A, left/right) and 0x17/0x18 (ADC B) are the input
digital volume of the four capsules (0.375 dB steps, 0x91 = 0 dB, 0xF1 =
+36 dB). Earlier releases only set 0x07 and 0x17, leaving two capsules at the
fixed +8.6 dB default; all four now follow the gain option.
"""
from log_util import log as _log

VOLUME_REGISTERS = (0x0007, 0x0008, 0x0017, 0x0018)
VOLUME_MAX = 0xF1   # +36 dB, the top of the documented range

INIT_SEQUENCE = (
    (0x0000, 0x0F), (0x0000, 0x00), (0x0010, 0x00), (0x0001, 0x09), (0x0002, 0xC0),
    (0x0003, 0xC4), (0x0004, 0x04), (0x0005, 0x0B), (0x0006, 0x00), (0x0007, 0xA8),
    (0x0008, 0xA8), (0x0009, 0x84), (0x000A, 0x00), (0x000B, 0xE1), (0x000C, 0x00),
    (0x000D, 0x00), (0x000E, 0x00), (0x000F, 0x00), (0x0011, 0x00), (0x0012, 0xC0),
    (0x0013, 0x04), (0x0014, 0x04), (0x0015, 0x00), (0x0016, 0x00), (0x0017, 0xA8),
    (0x0018, 0xA8), (0x0019, 0x80), (0x001A, 0x00), (0x001B, 0xE1), (0x001C, 0x00),
    (0x001D, 0x00), (0x001E, 0x00), (0x001F, 0x00), (0x0020, 0xA9), (0x0021, 0x1F),
    (0x0022, 0xAD), (0x0023, 0x20), (0x0024, 0x00), (0x0025, 0x00), (0x0026, 0x00),
    (0x0027, 0x00), (0x0028, 0x00), (0x0028, 0x00), (0x002A, 0x00), (0x002B, 0x00),
    (0x002C, 0x00), (0x002D, 0x00), (0x002E, 0xAD), (0x002F, 0x00), (0x0030, 0x09),
    (0x0031, 0x1F), (0x0032, 0x0D), (0x0033, 0x20), (0x0034, 0x00), (0x0035, 0x00),
    (0x0036, 0x00), (0x0037, 0x00), (0x0001, 0x08), (0x0003, 0xC4), (0x0005, 0x0B),
    (0x0000, 0x04), (0x0001, 0x09), (0x0000, 0x0C), (0x0010, 0x08), (0x0002, 0xC0),
    (0x0007, 0xA8), (0x0008, 0xA8), (0x0012, 0xC0), (0x0017, 0xA8), (0x0018, 0xA8),
    (0x0000, 0x0F), (0x0010, 0x0B),
)

_PIPE_STALL = 32   # EPIPE: the OV580 stalled endpoint zero


def log(level, message):
    _log(level, message, 'Audio')


def volume_register_value(gain_percent):
    """Map the 0-100 % option onto the ADC volume (same curve as earlier releases)."""
    gain_percent = max(0, min(100, int(gain_percent)))
    return min(VOLUME_MAX, 0x80 + gain_percent * 0x7F // 100)


def volume_db(register_value):
    return (register_value - 0x91) * 0.375


def _payload(register, value):
    return bytes((0x0B, 0, 0, 0, 0, 0, 0, 0, register & 0xFF, register >> 8, value, 0x24))


def configure(device, gain_percent):
    """Write the init sequence; recover once per register from an endpoint stall."""
    volume = volume_register_value(gain_percent)
    writes = stalls = 0
    for register, value in INIT_SEQUENCE:
        if register in VOLUME_REGISTERS:
            value = volume
        payload = _payload(register, value)
        try:
            result = device.ctrl_transfer(0x40, 0xA4, 0, 0, payload, timeout=1000)
        except OSError as exc:
            if exc.errno != _PIPE_STALL:
                raise RuntimeError(f'AK5703 register 0x{register:04x} failed after {writes} writes: {exc}') from exc
            stalls += 1
            try:
                device.clear_halt(0)
            except OSError:
                pass
            try:
                result = device.ctrl_transfer(0x40, 0xA4, 0, 0, payload, timeout=1000)
            except OSError as retry_exc:
                raise RuntimeError(f'AK5703 register 0x{register:04x} stalled twice ({retry_exc}). '
                                   'Close other camera programs and replug the camera.') from retry_exc
        if result != len(payload):
            raise RuntimeError(f'AK5703 short write at register 0x{register:04x}: {result}/{len(payload)}')
        writes += 1
    log('INFO', f'Microphone ADC started: {writes} register writes, capsule volume '
        f'{volume_db(volume):+.1f} dB ({gain_percent} %)' + (f', {stalls} USB stall(s) recovered' if stalls else ''))
    return {'adc_writes': writes, 'usb_stalls': stalls, 'volume_db': volume_db(volume)}
