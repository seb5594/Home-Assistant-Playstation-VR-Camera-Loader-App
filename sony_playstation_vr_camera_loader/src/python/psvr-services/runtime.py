"""Shared file locations and small helpers for the app services.

All services talk through files in RUN_DIR, so each one can restart on its
own without taking the others down:

    status.json   camera inventory (camera_loader)
    audio.json    microphone lifecycle (capture_service)
    engine.json   live capture/audio metrics (psvr-capture, once per second)
    still.jpg     newest preview image (psvr-capture, only while requested)
    video.json    picture controls (web UI -> psvr-capture)
    *.request     wishes from the web UI or MQTT to psvr-capture

Persistent files (firmware, learned camera models, picture controls) live in
CONFIG_DIR, the app's configuration folder.
"""
from contextlib import contextmanager
import fcntl
import json
import os
from pathlib import Path
import tempfile
import time

RUN_DIR = Path(os.environ.get('PS_CAMERA_RUN_DIR', '/run/ps-camera'))
CONFIG_DIR = Path(os.environ.get('PS_CAMERA_CONFIG_DIR', '/config'))

CAMERA_STATUS = RUN_DIR / 'status.json'
AUDIO_STATUS = RUN_DIR / 'audio.json'
ENGINE_STATUS = RUN_DIR / 'engine.json'
SNAPSHOT = RUN_DIR / 'still.jpg'
COMPARE_RAW = RUN_DIR / 'compare_raw.wav'
COMPARE_PROCESSED = RUN_DIR / 'compare_processed.wav'
ROWS_DUMP = RUN_DIR / 'rows.bin'

PREVIEW_REQUEST_SECONDS = 10

SONY_VENDOR = 0x05A9
BOOT_PID = 0x0580
UVC_PIDS = frozenset({0x058A, 0x058B, 0x058C})


def read_json(path, default=None):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return default


def write_json(path, value):
    """Replace the file atomically so readers never see half a document."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix=f'.{path.name}.')
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            json.dump(value, handle)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


@contextmanager
def locked(path):
    """Exclusive lock for read-modify-write of a shared file (threads and processes)."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path.with_name(path.name + '.lock'), 'a') as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        try:
            yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)


def request(name):
    """Touch a request file for psvr-capture (preview, compare, rows)."""
    RUN_DIR.mkdir(parents=True, exist_ok=True)
    (RUN_DIR / f'{name}.request').touch()


def request_preview():
    request('preview')


def preview_requested(now=None):
    try:
        age = (time.time() if now is None else now) - (RUN_DIR / 'preview.request').stat().st_mtime
    except OSError:
        return False
    return age < PREVIEW_REQUEST_SECONDS


def file_age(path):
    try:
        return time.time() - Path(path).stat().st_mtime
    except OSError:
        return None
