"""Camera firmware supplied by the user.

The app does not ship any firmware. The image is placed in the app's
configuration folder (/addon_configs/<slug>/ on the host, /config inside the
app) or uploaded on the app page, and is stored there as firmware_ps4.bin or
firmware_ps5.bin. Images the app has been tested with are recognised by their
SHA-256 checksum; only the checksums are part of the app.
"""
import argparse
from dataclasses import dataclass
import hashlib
import os
from pathlib import Path
import tempfile

from log_util import log
from runtime import CONFIG_DIR

CONFIG_ROOT = CONFIG_DIR
MODELS = ('ps4', 'ps5')
FILENAMES = {'ps4': 'firmware_ps4.bin', 'ps5': 'firmware_ps5.bin'}
MIN_SIZE = 16 * 1024
MAX_SIZE = 2 * 1024 * 1024
# Every OV580 camera firmware carries its USB product string.
OV580_SIGNATURE = 'USB Camera-OV580'.encode('utf-16-le')
KNOWN_IMAGES = {
    '09513d1068b9b5e23b09e03bca5640ed4d01830b034b9802395006b14a927b01': 'ps4',
    '10af1aee76fe0057a88db7ebf5f3ebf32430633effb93722be4cd0a9ed4fce54': 'ps5',
}


class FirmwareError(ValueError):
    """Rejected firmware image; `code` is translated by the web page."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


@dataclass(frozen=True)
class FirmwareImage:
    model: str
    path: Path
    sha256: str
    size: int

    @property
    def known(self):
        return KNOWN_IMAGES.get(self.sha256) == self.model

    def summary(self):
        return {'file': self.path.name, 'sha256': self.sha256[:16], 'size': self.size, 'known': self.known}


def check_image(data):
    """Raise FirmwareError unless `data` looks like an OV580 camera firmware."""
    if not MIN_SIZE <= len(data) <= MAX_SIZE:
        raise FirmwareError('size', f'Firmware must be {MIN_SIZE // 1024} KB to {MAX_SIZE // 1024 // 1024} MB, '
                                    f'got {len(data)} bytes')
    if OV580_SIGNATURE not in data:
        raise FirmwareError('signature', 'This file is not a PlayStation camera (OV580) firmware')


def _image(model, path):
    data = path.read_bytes()
    check_image(data)
    return FirmwareImage(model, path, hashlib.sha256(data).hexdigest(), len(data))


_checked = {}       # path -> ((mtime_ns, size), FirmwareImage or None)


def available(root=CONFIG_ROOT):
    """Usable firmware images by model; broken files are logged once and skipped.

    Files are only read again when they change, so this is cheap to call often.
    """
    images = {}
    for model, filename in FILENAMES.items():
        path = Path(root) / filename
        try:
            info = path.stat()
        except OSError:
            continue
        version = (info.st_mtime_ns, info.st_size)
        cached = _checked.get(path)
        if cached is None or cached[0] != version:
            try:
                image = _image(model, path)
            except (OSError, FirmwareError) as exc:
                log('WARNING', f'Ignoring {path.name}: {exc}')
                image = None
            cached = _checked[path] = (version, image)
        if cached[1] is not None:
            images[model] = cached[1]
    return images


def model_of(data, requested=None):
    """Model for an uploaded image: known checksums win over the user's choice."""
    known = KNOWN_IMAGES.get(hashlib.sha256(data).hexdigest())
    if known:
        return known
    if requested in MODELS:
        return requested
    raise FirmwareError('model', 'Unknown firmware version; choose whether it is for the PS4 or PS5 camera')


def _write_atomic(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix='.firmware-', delete=False) as temporary:
        try:
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
            os.replace(temporary.name, path)
        finally:
            if os.path.exists(temporary.name):
                os.unlink(temporary.name)


def store(data, requested=None, root=CONFIG_ROOT):
    """Validate and save an uploaded image; returns the stored FirmwareImage."""
    check_image(data)
    model = model_of(data, requested)
    path = Path(root) / FILENAMES[model]
    _write_atomic(path, data)
    image = FirmwareImage(model, path, hashlib.sha256(data).hexdigest(), len(data))
    log('INFO', f'Stored {model.upper()} firmware as {path.name} (sha256 {image.sha256[:16]}…, '
        f'{"known" if image.known else "unknown"} version)')
    return image


def adopt_custom_file(name, model, root=CONFIG_ROOT):
    """Copy a file from the removed firmware_file option to the standard name once."""
    root = Path(root)
    if not name or model not in MODELS or Path(name).name in FILENAMES.values():
        return None
    source = (root / name.removeprefix('/config/')).resolve()
    target = root / FILENAMES[model]
    if not source.is_relative_to(root.resolve()) or not source.is_file() or target.exists():
        return None
    _write_atomic(target, source.read_bytes())
    log('INFO', f'Copied {source.name} to {target.name}; the firmware file option is no longer needed')
    return target


def missing_message(models):
    names = ' or '.join(FILENAMES[model] for model in models)
    return (f'No camera firmware found. Upload it on the app page or copy {names} into the app '
            'configuration folder (/addon_configs/…_sony_playstation_vr_camera_loader).')


def main():
    """run.sh hook: carry a custom file from the removed firmware_file option over."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--adopt', required=True, metavar='FILE')
    parser.add_argument('--model', choices=MODELS, default='ps4')
    args = parser.parse_args()
    try:
        adopt_custom_file(args.adopt, args.model)
    except OSError as exc:
        log('WARNING', f'Could not copy {args.adopt}: {exc}')


if __name__ == '__main__':
    main()
