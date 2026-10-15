"""Which firmware a connected camera needs.

In boot mode the PS4 and the PS5 camera look identical on USB: both are the
OmniVision OV580 boot loader 05a9:0580, and even the product id after the
firmware upload comes from the firmware, not from the hardware. With
camera_model "auto" the app therefore learns instead of guessing:

  1. Only one firmware file present: that one is used.
  2. A camera on a known USB socket with the same boot fingerprint gets the
     firmware that produced pictures there last time.
  3. Unknown cameras start with PS4 (the far more common model). If the camera
     does not come back after the upload, or never delivers a frame, the
     other firmware is used on the next attempt.

The result is kept in /config/camera_models.json, so it survives updates.
"""
import time

from runtime import CONFIG_DIR, locked, read_json, write_json

MODELS = ('ps4', 'ps5')
STORE = CONFIG_DIR / 'camera_models.json'
FINGERPRINT_ATTRIBUTES = ('bcdDevice', 'version', 'speed', 'bMaxPower', 'manufacturer', 'product', 'serial')

TRYING, WORKING, FAILED = 'trying', 'working', 'failed'


def boot_fingerprint(sysfs_entry):
    """Descriptor values of the boot loader; equal for identical hardware."""
    values = []
    for name in FINGERPRINT_ATTRIBUTES:
        try:
            values.append(f'{name}={(sysfs_entry / name).read_text().strip()}')
        except OSError:
            continue
    return ';'.join(values)


def port_key(bus, ports):
    return f'{bus}-' + '.'.join(str(port) for port in ports)


def other(model):
    return MODELS[1 - MODELS.index(model)]


class ModelMemory:
    def __init__(self, path=STORE, clock=time.time):
        self.path = path
        self.clock = clock

    def _sockets(self):
        return (read_json(self.path, {}) or {}).get('sockets', {})

    def _save(self, sockets):
        write_json(self.path, {'sockets': sockets})

    def record(self, port):
        return self._sockets().get(port)

    def choose(self, port, fingerprint, available, configured='auto'):
        """Model to upload, or None when no usable firmware exists."""
        if configured in MODELS:
            return configured if configured in available else None
        if len(available) <= 1:
            return next(iter(available), None)
        sockets = self._sockets()
        previous = sockets.get(port)
        if previous and previous.get('fingerprint') == fingerprint:
            # Back in boot mode while still "trying" means that attempt did not work out.
            return previous['model'] if previous['state'] == WORKING else other(previous['model'])
        learned = {record['model'] for record in sockets.values()
                   if record.get('fingerprint') == fingerprint and record['state'] == WORKING}
        return learned.pop() if len(learned) == 1 else MODELS[0]

    def _update(self, port, **values):
        # camera_loader and capture_service both write here
        with locked(self.path):
            sockets = self._sockets()
            record = sockets.get(port, {})
            record.update(values, updated=self.clock())
            sockets[port] = record
            self._save(sockets)
        return record

    def attempt(self, port, fingerprint, model):
        return self._update(port, fingerprint=fingerprint, model=model, state=TRYING)

    def confirm(self, port):
        """The camera on `port` delivered frames with the firmware it got."""
        record = self.record(port)
        if record and record['state'] != WORKING:
            return self._update(port, state=WORKING)
        return record

    def fail(self, port):
        """The last upload did not give a working camera; returns the next model."""
        record = self.record(port)
        if not record:
            return None
        self._update(port, state=FAILED)
        return other(record['model'])

    def model_for(self, port):
        record = self.record(port)
        return record['model'] if record and record['state'] in (TRYING, WORKING) else None
