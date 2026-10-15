#!/usr/bin/env python3
"""Upload firmware to PlayStation cameras in boot mode and publish their state.

A fresh camera enumerates as 05a9:0580 (boot loader). After the firmware
upload it re-enumerates as a UVC camera on the same physical USB socket.
Which firmware a camera needs is decided by camera_models (camera_model
"auto") or by the configuration. This service reacts to hotplug events only;
it does not poll, except while a camera waits for a firmware file.
"""
import argparse
import hashlib
import time

from camera_models import MODELS, ModelMemory, boot_fingerprint, port_key
import firmware_manager
from device_events import DeviceWatcher
from log_util import log
from runtime import BOOT_PID, CAMERA_STATUS, UVC_PIDS, write_json
from usb_device import DISCONNECT_ERRORS, format_port, inventory
from video_nodes import camera_nodes

FIRMWARE_CHUNK = 512
REENUMERATION_TIMEOUT = 30
RETRY_AFTER_FAILURE = 30
RETRY_OTHER_MODEL = 2
FIRMWARE_RECHECK = 5            # seconds between looks for an uploaded firmware file
EVENT_DEBOUNCE = 0.25
SETTLE_CHECKS = (2, 8)          # seconds; covers a late /sys or /dev mount at start


class ReenumerationTimeout(Exception):
    """The camera accepted the firmware upload but never came back as a UVC device."""


def wait_for_uvc(path, watcher, timeout=REENUMERATION_TIMEOUT, scan=inventory, clock=time.monotonic):
    """Wait for the camera on `path` to come back with a UVC product id."""
    deadline = clock() + timeout
    while True:
        device = scan().get(path)
        if device is not None and device.idProduct in UVC_PIDS:
            return device
        remaining = deadline - clock()
        if remaining <= 0:
            return None
        watcher.wait(min(remaining, 1.0))


def upload_firmware(device, firmware, watcher, scan=inventory, timeout=REENUMERATION_TIMEOUT):
    data = firmware.read_bytes()
    if not 1024 <= len(data) <= 2 * 1024 * 1024:
        raise ValueError(f'Invalid firmware size: {len(data)} bytes')
    port = format_port(*device.path)
    log('INFO', f'Uploading {firmware.name} ({len(data)} bytes, sha256 '
        f'{hashlib.sha256(data).hexdigest()[:16]}…) to USB {port}')
    with device:
        device.set_configuration()
        for offset in range(0, len(data), FIRMWARE_CHUNK):
            chunk = data[offset:offset + FIRMWARE_CHUNK]
            count = device.ctrl_transfer(0x40, 0, offset & 0xFFFF, 0x14 + (offset >> 16), chunk, timeout=3000)
            if count != len(chunk):
                raise OSError(f'Short firmware transfer at offset {offset}: {count}/{len(chunk)}')
        try:
            device.ctrl_transfer(0x40, 0, 0x2200, 0x8018, b'\x5b', timeout=3000)
        except OSError as exc:
            # The camera may drop off the bus before acknowledging the reboot.
            if exc.errno not in DISCONNECT_ERRORS:
                raise
    initialized = wait_for_uvc(device.path, watcher, timeout, scan=scan)
    if initialized is None:
        raise ReenumerationTimeout(f'Camera on USB {port} did not return as a UVC device after the firmware upload')
    log('INFO', f'Camera on USB {port} is running firmware as 05a9:{initialized.idProduct:04x}')
    return initialized


def build_status(devices, setting, images, memory, error=None, nodes_by_path=None):
    nodes_by_path = camera_nodes() if nodes_by_path is None else nodes_by_path
    initialized = []
    for device in devices:
        if device.idProduct not in UVC_PIDS:
            continue
        initialized.append({'pid': f'{device.idProduct:04x}', 'bus': device.bus,
                            'ports': list(device.port_numbers),
                            'model': memory.model_for(port_key(device.bus, device.port_numbers)),
                            'video_nodes': nodes_by_path.get(device.path, [])})
    return {
        'camera_model': setting,
        'firmware': {model: image.summary() for model, image in images.items()},
        'bootloaders': sum(device.idProduct == BOOT_PID for device in devices),
        'initialized': initialized,
        'initialized_count': len(initialized),
        'error': error,
        'updated': time.time(),
    }


class CameraTracker:
    """Re-inventory after hotplug events, with one retry timer per boot device."""

    def __init__(self, setting, watcher, memory=None, firmware_root=firmware_manager.CONFIG_ROOT,
                 clock=time.monotonic, scan=inventory):
        self.setting = setting
        self.watcher = watcher
        self.memory = memory or ModelMemory()
        self.firmware_root = firmware_root
        self.clock = clock
        self.scan = scan
        self.images = {}
        self.retry_at = {}
        self.settle_index = 0
        self.refresh_at = None
        self.previous = None

    def _upload(self, device):
        """Upload the right firmware to one boot device; returns an error dict or None."""
        port = port_key(*device.path)
        fingerprint = boot_fingerprint(device.sysfs_entry)
        model = self.memory.choose(port, fingerprint, self.images, self.setting)
        if model is None:
            wanted = [self.setting] if self.setting in MODELS else list(MODELS)
            self.retry_at[device.path] = self.clock() + FIRMWARE_RECHECK
            return {'code': 'firmware_missing', 'message': firmware_manager.missing_message(wanted)}
        self.memory.attempt(port, fingerprint, model)
        try:
            upload_firmware(device, self.images[model].path, self.watcher, self.scan)
        except ReenumerationTimeout as exc:
            if self.setting == 'auto' and len(self.images) > 1:
                following = self.memory.fail(port)
                log('WARNING', f'Camera on USB {port} did not start with the {model.upper()} firmware; '
                    f'trying the {following.upper()} firmware next')
                self.retry_at[device.path] = self.clock() + RETRY_OTHER_MODEL
            else:
                self.retry_at[device.path] = self.clock() + RETRY_AFTER_FAILURE
            return {'code': 'firmware_timeout', 'message': str(exc)}
        except (OSError, ValueError) as exc:
            self.retry_at[device.path] = self.clock() + RETRY_AFTER_FAILURE
            log('ERROR', f'Firmware upload failed on USB {port}: {exc}; retrying in {RETRY_AFTER_FAILURE}s')
            return {'code': 'firmware_upload', 'message': str(exc)}
        self.retry_at.pop(device.path, None)
        return None

    def _upload_pending(self, devices):
        error = None
        waiting = [device for device in devices if device.idProduct == BOOT_PID]
        for device in waiting:
            if self.clock() >= self.retry_at.get(device.path, 0):
                error = self._upload(device) or error
        return error

    def refresh(self):
        self.images = firmware_manager.available(self.firmware_root)   # cached until a file changes
        devices = list(self.scan().values())
        boot_paths = {device.path for device in devices if device.idProduct == BOOT_PID}
        self.retry_at = {path: due for path, due in self.retry_at.items() if path in boot_paths}
        error = self._upload_pending(devices)
        if boot_paths:
            devices = list(self.scan().values())
        status = build_status(devices, self.setting, self.images, self.memory, error)
        write_json(CAMERA_STATUS, status)

        if devices:
            self.settle_index = len(SETTLE_CHECKS)
        elif self.settle_index < len(SETTLE_CHECKS):
            self.refresh_at = self.clock() + SETTLE_CHECKS[self.settle_index]
            self.settle_index += 1
        elif self.settle_index == len(SETTLE_CHECKS):
            log('INFO', 'No PlayStation camera found yet. Waiting for it to be plugged in '
                '(expected USB id 05a9:0580 in /sys/bus/usb/devices).')
            self.settle_index += 1
        self._log_changes(status)
        return status

    def _log_changes(self, status):
        summary = (status['bootloaders'], tuple((c['bus'], tuple(c['ports']), c['pid'], tuple(c['video_nodes']))
                                                for c in status['initialized']), status['error'])
        if summary == self.previous:
            return
        self.previous = summary
        log('INFO', f'Cameras: {status["initialized_count"]} ready, {status["bootloaders"]} waiting for firmware')
        for camera in status['initialized']:
            nodes = ', '.join(camera['video_nodes']) or 'pending'
            model = f'{camera["model"].upper()} firmware, ' if camera['model'] else ''
            log('INFO', f'  05a9:{camera["pid"]} on USB {format_port(camera["bus"], camera["ports"])}, '
                f'{model}video {nodes}')
        if status['error'] and status['error']['code'] == 'firmware_missing':
            log('WARNING', status['error']['message'])

    def next_deadline(self):
        deadlines = [due for due in (self.refresh_at,) if due is not None]
        deadlines.extend(self.retry_at.values())
        return min(deadlines) if deadlines else None

    def run(self):
        self.refresh()
        while True:
            deadline = self.next_deadline()
            timeout = None if deadline is None else max(0.0, deadline - self.clock())
            if self.watcher.wait(timeout):
                # Collect the burst of nodes one replug creates, then scan once.
                self.refresh_at = self.clock() + EVENT_DEBOUNCE
                continue
            if deadline is not None and self.clock() >= deadline:
                self.refresh_at = None
                self.refresh()


def monitor(setting):
    while True:
        try:
            watcher = DeviceWatcher()
        except OSError as exc:
            log('ERROR', f'Hotplug watcher unavailable: {exc}; retrying in 5s')
            write_json(CAMERA_STATUS, build_status([], setting, {}, ModelMemory(),
                                                   {'code': 'watcher', 'message': str(exc)}, {}))
            time.sleep(5)
            continue
        log('INFO', 'Waiting for camera hotplug events on /dev')
        try:
            CameraTracker(setting, watcher).run()
        except OSError as exc:
            log('ERROR', f'Camera inventory failed: {exc}; restarting in 5s')
            time.sleep(5)
        finally:
            watcher.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--model', choices=('auto',) + MODELS, default='auto')
    args = parser.parse_args()
    monitor(args.model)


if __name__ == '__main__':
    main()
