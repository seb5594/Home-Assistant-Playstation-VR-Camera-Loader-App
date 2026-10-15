"""Wake up on USB and video hotplug without polling.

Kernel and udev netlink events never reach an app container (it has its own
network namespace), but the host's /dev is bind-mounted. inotify on /dev and
/dev/bus/usb/NNN therefore sees every camera add, remove and re-enumeration.
"""
import ctypes
import os
from pathlib import Path
import select
import struct

_EVENT = struct.Struct('iIII')
_IN_CREATE = 0x00000100
_IN_DELETE = 0x00000200
_IN_MOVED_FROM = 0x00000040
_IN_MOVED_TO = 0x00000080
_IN_Q_OVERFLOW = 0x00004000
_IN_IGNORED = 0x00008000
_CHANGES = _IN_CREATE | _IN_DELETE | _IN_MOVED_FROM | _IN_MOVED_TO


class DeviceWatcher:
    """inotify watch on /dev (video nodes) and every /dev/bus/usb/NNN directory."""

    sources = ('device nodes',)

    def __init__(self, root=Path('/dev')):
        self.root = Path(root)
        self.usb_root = self.root / 'bus' / 'usb'
        libc = ctypes.CDLL(None, use_errno=True)
        self._add_watch = libc.inotify_add_watch
        self._add_watch.argtypes = [ctypes.c_int, ctypes.c_char_p, ctypes.c_uint32]
        self._add_watch.restype = ctypes.c_int
        libc.inotify_init1.argtypes = [ctypes.c_int]
        self.fd = libc.inotify_init1(os.O_NONBLOCK | os.O_CLOEXEC)
        if self.fd < 0:
            raise OSError(ctypes.get_errno(), 'Could not initialize device node watcher')
        self.paths = {}
        try:
            if not self._watch(self.root):
                raise OSError(f'Device directory is unavailable: {self.root}')
            self._watch_usb_tree()
        except Exception:
            self.close()
            raise

    def _watch(self, path):
        if not path.is_dir() or path in self.paths.values():
            return False
        wd = self._add_watch(self.fd, os.fsencode(path), _CHANGES)
        if wd < 0:
            raise OSError(ctypes.get_errno(), f'Cannot watch {path}')
        self.paths[wd] = path
        return True

    def _watch_usb_tree(self):
        self._watch(self.root / 'bus')
        self._watch(self.usb_root)
        if self.usb_root.is_dir():
            for bus in self.usb_root.iterdir():
                if bus.name.isdigit():
                    self._watch(bus)

    def fileno(self):
        return self.fd

    def _drain(self):
        """Read queued events; True if one of them concerns cameras."""
        try:
            data = os.read(self.fd, 65536)
        except BlockingIOError:
            return False
        changed = False
        offset = 0
        while offset + _EVENT.size <= len(data):
            wd, mask, _cookie, length = _EVENT.unpack_from(data, offset)
            offset += _EVENT.size
            name = data[offset:offset + length].split(b'\0', 1)[0].decode(errors='replace')
            offset += length
            if mask & _IN_IGNORED:
                self.paths.pop(wd, None)
                continue
            path = self.paths.get(wd)
            if mask & _IN_Q_OVERFLOW:
                changed = True
            elif path == self.root and name.startswith('video') and name[5:].isdigit():
                changed = True
            elif (path == self.root and name == 'bus') or (path == self.root / 'bus' and name == 'usb'):
                self._watch_usb_tree()
                changed = True
            elif path == self.usb_root and name.isdigit():
                self._watch(self.usb_root / name)
                changed = True
            elif path is not None and path.parent == self.usb_root and name.isdigit():
                changed = True
        return changed

    def wait(self, timeout=None):
        """Block up to `timeout` seconds; True if a camera-related node changed."""
        readable, _, _ = select.select([self.fd], [], [], timeout)
        return bool(readable) and self._drain()

    def close(self):
        if self.fd >= 0:
            os.close(self.fd)
            self.fd = -1
