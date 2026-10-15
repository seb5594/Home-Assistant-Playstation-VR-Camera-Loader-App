"""Find Sony camera USB devices in sysfs and talk to them through usbfs.

Home Assistant apps run in their own network namespace, so libusb/udev
hotplug and even PyUSB enumeration are unreliable there (alpha10 saw the boot
device in sysfs but not in PyUSB). Everything here uses the host's
/sys/bus/usb/devices and /dev/bus/usb directly: one code path, no libusb.
"""
import ctypes
import errno
import fcntl
import os
from pathlib import Path
import struct

from runtime import BOOT_PID, SONY_VENDOR, UVC_PIDS

USB_ROOT = Path('/sys/bus/usb/devices')
DEV_ROOT = Path('/dev/bus/usb')
SUPPORTED_PIDS = UVC_PIDS | {BOOT_PID}

# Errors a control transfer can raise while the camera re-enumerates.
DISCONNECT_ERRORS = frozenset({errno.ENODEV, errno.EPROTO, errno.ESHUTDOWN,
                               errno.EPIPE, errno.ETIMEDOUT, errno.EIO})


class _ControlTransfer(ctypes.Structure):
    """struct usbdevfs_ctrltransfer from <linux/usbdevice_fs.h>."""

    _fields_ = [('request_type', ctypes.c_uint8), ('request', ctypes.c_uint8),
                ('value', ctypes.c_uint16), ('index', ctypes.c_uint16),
                ('length', ctypes.c_uint16), ('timeout', ctypes.c_uint32),
                ('data', ctypes.c_void_p)]


def _ioctl_code(direction, number, size):
    return (direction << 30) | (size << 16) | (ord('U') << 8) | number


USBDEVFS_CONTROL = _ioctl_code(3, 0, ctypes.sizeof(_ControlTransfer))
USBDEVFS_SETCONFIGURATION = _ioctl_code(2, 5, ctypes.sizeof(ctypes.c_uint))
USBDEVFS_CLEAR_HALT = _ioctl_code(2, 21, ctypes.sizeof(ctypes.c_uint))


def _read_int(path, base=10):
    return int(path.read_text().strip(), base)


def usb_path_of(sysfs_device):
    """(bus, ports) identifies the physical socket across re-enumeration."""
    bus = _read_int(sysfs_device / 'busnum')
    ports = tuple(int(port) for port in (sysfs_device / 'devpath').read_text().strip().split('.'))
    return bus, ports


def format_port(bus, ports):
    return f'{bus}-' + '.'.join(str(port) for port in ports)


class UsbDevice:
    """One camera USB device; opens its usbfs node only for control transfers."""

    def __init__(self, sysfs_entry, product_id, bus, address, ports, dev_root=DEV_ROOT):
        self.sysfs_entry = Path(sysfs_entry)
        self.idVendor = SONY_VENDOR
        self.idProduct = product_id
        self.bus = bus
        self.address = address
        self.port_numbers = tuple(ports)
        self.node = Path(dev_root) / f'{bus:03d}' / f'{address:03d}'
        self._fd = None

    @property
    def path(self):
        return self.bus, self.port_numbers

    def _open(self):
        if self._fd is None:
            try:
                self._fd = os.open(self.node, os.O_RDWR | os.O_CLOEXEC)
            except OSError as exc:
                raise OSError(exc.errno, f'Camera USB node {self.node} is unavailable: {exc.strerror}') from exc
        return self._fd

    def set_configuration(self, value=1):
        try:
            if _read_int(self.sysfs_entry / 'bConfigurationValue') == value:
                return
        except (OSError, ValueError):
            pass
        fcntl.ioctl(self._open(), USBDEVFS_SETCONFIGURATION, struct.pack('=I', value))

    def ctrl_transfer(self, request_type, request, value, index, payload, timeout=1000):
        """Returns bytes for IN transfers and the written length for OUT transfers."""
        if isinstance(payload, int):
            buffer = ctypes.create_string_buffer(payload)
            length = payload
        else:
            data = bytes(payload)
            length = len(data)
            buffer = ctypes.create_string_buffer(data, max(1, length))
        transfer = _ControlTransfer(request_type, request, value, index, length, timeout,
                                    ctypes.addressof(buffer))
        result = fcntl.ioctl(self._open(), USBDEVFS_CONTROL, bytearray(bytes(transfer)), True)
        return bytes(buffer)[:result] if request_type & 0x80 else result

    def clear_halt(self, endpoint):
        fcntl.ioctl(self._open(), USBDEVFS_CLEAR_HALT, struct.pack('=I', endpoint))

    def close(self):
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None

    def __enter__(self):
        return self

    def __exit__(self, *_exc):
        self.close()


def inventory(root=USB_ROOT, dev_root=DEV_ROOT):
    """All Sony boot-mode and UVC cameras keyed by physical (bus, ports)."""
    found = {}
    for entry in Path(root).glob('*'):
        try:
            if _read_int(entry / 'idVendor', 16) != SONY_VENDOR:
                continue
            product_id = _read_int(entry / 'idProduct', 16)
            if product_id not in SUPPORTED_PIDS:
                continue
            bus, ports = usb_path_of(entry)
            address = _read_int(entry / 'devnum')
        except (OSError, ValueError):
            continue  # interfaces (1-2:1.0) and devices that vanished mid-scan
        found[(bus, ports)] = UsbDevice(entry, product_id, bus, address, ports, dev_root)
    return found


def find(product_id, bus, ports):
    device = inventory().get((bus, tuple(ports)))
    return device if device is not None and device.idProduct == product_id else None
