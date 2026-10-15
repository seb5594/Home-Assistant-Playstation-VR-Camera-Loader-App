"""USB inventory, usbfs transfers, video node lookup and hotplug detection."""
import ctypes
import errno
import hashlib
from pathlib import Path
import tempfile
import time
import unittest
from unittest.mock import patch

import helpers  # noqa: F401  (sets up paths)
import camera_loader
from camera_models import ModelMemory, boot_fingerprint
import firmware_manager
from device_events import DeviceWatcher
import usb_device
import video_nodes


class FakeWatcher:
    def __init__(self, events=()):
        self.events = list(events)
        self.waits = 0

    def wait(self, timeout=None):
        self.waits += 1
        if self.events:
            return self.events.pop(0)
        time.sleep(min(timeout or 0, 0.01))
        return False


class UsbInventoryTests(unittest.TestCase):
    def test_boot_and_uvc_devices_are_found_by_physical_port(self):
        with tempfile.TemporaryDirectory() as root:
            helpers.make_usb_device(root, '3-2', 0x0580, 3, '2', 2)
            helpers.make_usb_device(root, '1-1.4', 0x058A, 1, '1.4', 7)
            (Path(root) / '3-2:1.0').mkdir()                         # interface, ignored
            other = helpers.make_usb_device(root, '2-1', 0x1234, 2, '1', 3)
            (other / 'idVendor').write_text('046d\n')                 # not Sony
            found = usb_device.inventory(root, Path(root) / 'dev')
        self.assertEqual(set(found), {(3, (2,)), (1, (1, 4))})
        self.assertEqual(found[(3, (2,))].idProduct, 0x0580)
        self.assertEqual(found[(1, (1, 4))].node.name, '007')

    def test_control_struct_matches_kernel_abi(self):
        expected = 24 if ctypes.sizeof(ctypes.c_void_p) == 8 else 16
        self.assertEqual(ctypes.sizeof(usb_device._ControlTransfer), expected)
        self.assertEqual(usb_device.USBDEVFS_CONTROL & 0xFF, 0)
        self.assertEqual((usb_device.USBDEVFS_CONTROL >> 8) & 0xFF, ord('U'))

    def test_control_transfer_passes_payload_to_kernel(self):
        captured = {}

        def fake_ioctl(fd, request, buffer, mutate=False):
            transfer = usb_device._ControlTransfer.from_buffer(buffer)
            captured.update(type=transfer.request_type, request=transfer.request, value=transfer.value,
                            index=transfer.index, data=ctypes.string_at(transfer.data, transfer.length))
            return transfer.length

        with tempfile.TemporaryDirectory() as root:
            entry = helpers.make_usb_device(root, '3-2', 0x0580, 3, '2', 2)
            device = usb_device.UsbDevice(entry, 0x0580, 3, 2, (2,), Path(root))
            with patch.object(usb_device.os, 'open', return_value=99), \
                    patch.object(usb_device.os, 'close'), \
                    patch.object(usb_device.fcntl, 'ioctl', side_effect=fake_ioctl):
                with device:
                    self.assertEqual(device.ctrl_transfer(0x40, 0, 0x1200, 0x14, b'\x01\x02\x03'), 3)
        self.assertEqual(captured, {'type': 0x40, 'request': 0, 'value': 0x1200, 'index': 0x14,
                                    'data': b'\x01\x02\x03'})


class FirmwareUploadTests(unittest.TestCase):
    class BootDevice:
        idProduct = 0x0580
        bus, port_numbers = 3, (2,)
        path = (3, (2,))

        def __init__(self, reboot_error=errno.ENODEV):
            self.transfers = []
            self.reboot_error = reboot_error
            self.closed = False

        def __enter__(self):
            return self

        def __exit__(self, *_):
            self.closed = True

        def set_configuration(self):
            pass

        def ctrl_transfer(self, request_type, request, value, index, payload, timeout=1000):
            if (value, index) == (0x2200, 0x8018):
                raise OSError(self.reboot_error, 'gone')
            self.transfers.append((value, index, bytes(payload)))
            return len(payload)

    class Ready:
        idProduct = 0x058A

    def test_firmware_is_chunked_and_reboot_disconnect_is_expected(self):
        with tempfile.TemporaryDirectory() as folder:
            firmware = Path(folder) / 'firmware.bin'
            firmware.write_bytes(bytes(range(256)) * 9)          # 2304 bytes
            for error in (errno.ENODEV, errno.EPROTO, errno.ESHUTDOWN):
                device = self.BootDevice(error)
                result = camera_loader.upload_firmware(device, firmware, FakeWatcher(),
                                                       scan=lambda: {(3, (2,)): self.Ready()})
                self.assertEqual(result.idProduct, 0x058A)
                self.assertEqual([len(t[2]) for t in device.transfers], [512, 512, 512, 512, 256])
                self.assertEqual(device.transfers[1][:2], (512, 0x14))
                self.assertTrue(device.closed)

    def test_upload_times_out_without_reenumeration(self):
        with tempfile.TemporaryDirectory() as folder:
            firmware = Path(folder) / 'firmware.bin'
            firmware.write_bytes(b'\0' * 2048)
            with self.assertRaises(camera_loader.ReenumerationTimeout):
                camera_loader.upload_firmware(self.BootDevice(), firmware, FakeWatcher(), scan=dict, timeout=0.2)

    def test_unrelated_transfer_errors_are_not_hidden(self):
        with tempfile.TemporaryDirectory() as folder:
            firmware = Path(folder) / 'firmware.bin'
            firmware.write_bytes(b'\0' * 2048)
            with self.assertRaises(OSError):
                camera_loader.upload_firmware(self.BootDevice(errno.EACCES), firmware, FakeWatcher(), scan=dict)


class StatusTests(unittest.TestCase):
    class Device:
        def __init__(self, pid, bus, ports):
            self.idProduct, self.bus, self.port_numbers = pid, bus, ports
            self.path = (bus, ports)

    def test_initialized_camera_carries_nodes_and_learned_model(self):
        with tempfile.TemporaryDirectory() as folder:
            memory = ModelMemory(Path(folder) / 'models.json')
            memory.attempt('3-2', 'boot', 'ps5')
            devices = [self.Device(0x058A, 3, (2,))]
            status = camera_loader.build_status(devices, 'auto', {}, memory,
                                                nodes_by_path={(3, (2,)): ['/dev/video0']})
        camera = status['initialized'][0]
        self.assertEqual((camera['video_nodes'], camera['model']), (['/dev/video0'], 'ps5'))
        self.assertEqual(status['camera_model'], 'auto')

    def test_bootloader_counts_but_is_not_initialized(self):
        with tempfile.TemporaryDirectory() as folder:
            status = camera_loader.build_status([self.Device(0x0580, 3, (2,))], 'ps4', {},
                                                ModelMemory(Path(folder) / 'm.json'), nodes_by_path={})
        self.assertEqual((status['bootloaders'], status['initialized_count']), (1, 0))


def firmware_image(size=40000, marker=b''):
    return marker + firmware_manager.OV580_SIGNATURE + bytes(size - len(marker) - len(firmware_manager.OV580_SIGNATURE))


class FirmwareFileTests(unittest.TestCase):
    def test_upload_is_checked_and_stored_under_the_model_name(self):
        with tempfile.TemporaryDirectory() as folder:
            image = firmware_manager.store(firmware_image(), 'ps5', root=Path(folder))
            self.assertEqual(image.path.name, 'firmware_ps5.bin')
            self.assertFalse(image.known)
            self.assertEqual(set(firmware_manager.available(Path(folder))), {'ps5'})

    def test_known_checksum_wins_over_the_chosen_model(self):
        data = firmware_image()
        with patch.dict(firmware_manager.KNOWN_IMAGES, {hashlib.sha256(data).hexdigest(): 'ps4'}):
            with tempfile.TemporaryDirectory() as folder:
                image = firmware_manager.store(data, 'ps5', root=Path(folder))
                self.assertEqual((image.model, image.known), ('ps4', True))

    def test_unknown_image_needs_a_model_and_junk_is_rejected(self):
        with tempfile.TemporaryDirectory() as folder:
            for data, model, code in ((firmware_image(), None, 'model'), (b'PK' + bytes(40000), 'ps4', 'signature'),
                                      (firmware_image(4000), 'ps4', 'size')):
                with self.assertRaises(firmware_manager.FirmwareError) as caught:
                    firmware_manager.store(data, model, root=Path(folder))
                self.assertEqual(caught.exception.code, code)
            self.assertEqual(list(Path(folder).iterdir()), [])

    def test_custom_file_of_the_removed_option_is_adopted_once(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / 'my_camera.bin').write_bytes(firmware_image())
            self.assertEqual(firmware_manager.adopt_custom_file('my_camera.bin', 'ps4', root).name, 'firmware_ps4.bin')
            self.assertIsNone(firmware_manager.adopt_custom_file('my_camera.bin', 'ps4', root))
            self.assertIsNone(firmware_manager.adopt_custom_file('../etc/passwd', 'ps5', root))


class ModelMemoryTests(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.memory = ModelMemory(Path(self.folder.name) / 'camera_models.json')
        self.both = {'ps4': object(), 'ps5': object()}

    def tearDown(self):
        self.folder.cleanup()

    def test_single_firmware_and_forced_model(self):
        self.assertEqual(self.memory.choose('3-2', 'boot', {'ps5': 1}), 'ps5')
        self.assertEqual(self.memory.choose('3-2', 'boot', self.both, 'ps5'), 'ps5')
        self.assertIsNone(self.memory.choose('3-2', 'boot', {'ps4': 1}, 'ps5'))
        self.assertIsNone(self.memory.choose('3-2', 'boot', {}))

    def test_unknown_camera_starts_with_ps4_and_switches_after_a_failed_attempt(self):
        self.assertEqual(self.memory.choose('3-2', 'boot', self.both), 'ps4')
        self.memory.attempt('3-2', 'boot', 'ps4')
        # back in boot mode without a confirmed picture: the attempt did not work out
        self.assertEqual(self.memory.choose('3-2', 'boot', self.both), 'ps5')
        self.memory.attempt('3-2', 'boot', 'ps5')
        self.memory.confirm('3-2')
        self.assertEqual(self.memory.choose('3-2', 'boot', self.both), 'ps5')
        self.assertEqual(self.memory.model_for('3-2'), 'ps5')

    def test_explicit_failure_and_learning_across_sockets(self):
        self.memory.attempt('3-2', 'boot-a', 'ps5')
        self.memory.confirm('3-2')
        self.assertEqual(self.memory.choose('1-4', 'boot-a', self.both), 'ps5')   # same fingerprint
        self.assertEqual(self.memory.choose('1-4', 'boot-b', self.both), 'ps4')   # other hardware
        self.assertEqual(self.memory.fail('3-2'), 'ps4')
        self.assertIsNone(self.memory.model_for('3-2'))

    def test_fingerprint_reads_boot_descriptors(self):
        entry = Path(self.folder.name) / '3-2'
        entry.mkdir()
        (entry / 'bcdDevice').write_text('0100\n')
        (entry / 'product').write_text('USB Boot\n')
        self.assertEqual(boot_fingerprint(entry), 'bcdDevice=0100;product=USB Boot')


class TrackerFirmwareTests(unittest.TestCase):
    class BootDevice:
        idProduct = 0x0580

        def __init__(self, sysfs_entry):
            self.sysfs_entry = sysfs_entry
            self.path = (3, (2,))
            self.bus, self.port_numbers = 3, (2,)

    def make_tracker(self, folder, images, clock=lambda: 100.0):
        root = Path(folder)
        for model in images:
            (root / firmware_manager.FILENAMES[model]).write_bytes(firmware_image(marker=model.encode()))
        memory = ModelMemory(root / 'camera_models.json')
        tracker = camera_loader.CameraTracker('auto', FakeWatcher(), memory, firmware_root=root, clock=clock)
        tracker.images = firmware_manager.available(root)     # what refresh() does first
        return tracker, memory, self.BootDevice(root)

    def test_usb_timeouts_during_the_transfer_do_not_switch_the_model(self):
        def flaky_upload(device, firmware, watcher, scan):
            raise TimeoutError(110, 'Connection timed out')     # ETIMEDOUT from a control transfer

        with tempfile.TemporaryDirectory() as folder:
            tracker, memory, device = self.make_tracker(folder, ('ps4', 'ps5'))
            with patch.object(camera_loader, 'upload_firmware', flaky_upload):
                error = tracker._upload_pending([device])
            self.assertEqual(error['code'], 'firmware_upload')
            self.assertEqual(memory.record('3-2')['state'], 'trying')

    def test_missing_firmware_is_reported_and_rechecked(self):
        with tempfile.TemporaryDirectory() as folder:
            tracker, _memory, device = self.make_tracker(folder, ())
            error = tracker._upload_pending([device])
        self.assertEqual(error['code'], 'firmware_missing')
        self.assertEqual(tracker.retry_at[device.path], 100.0 + camera_loader.FIRMWARE_RECHECK)

    def test_camera_that_does_not_return_gets_the_other_firmware_next(self):
        uploads = []

        def fake_upload(device, firmware, watcher, scan):
            uploads.append(firmware.name)
            raise camera_loader.ReenumerationTimeout('did not return')

        with tempfile.TemporaryDirectory() as folder:
            tracker, memory, device = self.make_tracker(folder, ('ps4', 'ps5'))
            with patch.object(camera_loader, 'upload_firmware', fake_upload):
                error = tracker._upload_pending([device])
                self.assertEqual(error['code'], 'firmware_timeout')
                tracker.retry_at.clear()
                tracker._upload_pending([device])
        self.assertEqual(uploads, ['firmware_ps4.bin', 'firmware_ps5.bin'])


class VideoNodeTests(unittest.TestCase):
    def make_tree(self, root, product_id=0x058A):
        root = Path(root)
        usb = helpers.make_usb_device(root / 'devices', '3-2', product_id, 3, '2', 5)
        video_class = root / 'class'
        video_class.mkdir()
        dev = root / 'dev'
        dev.mkdir()
        for name, index in (('video0', 0), ('video1', 1)):
            entry = usb / '3-2:1.0' / 'video4linux' / name
            entry.mkdir(parents=True)
            (entry / 'index').write_text(f'{index}\n')
            (video_class / name).symlink_to(entry)
            (dev / name).touch()
        return video_class, dev

    def test_metadata_node_is_skipped_and_capture_node_kept(self):
        with tempfile.TemporaryDirectory() as root:
            video_class, dev = self.make_tree(root)
            nodes = video_nodes.camera_nodes(video_class, dev, probe=lambda node: True)
        self.assertEqual(list(nodes), [(3, (2,))])
        self.assertEqual([Path(n).name for n in nodes[(3, (2,))]], ['video0'])

    def test_node_must_still_belong_to_the_same_camera(self):
        with tempfile.TemporaryDirectory() as root:
            video_class, dev = self.make_tree(root)
            node = str(dev / 'video0')
            self.assertTrue(video_nodes.node_matches(node, 3, [2], 0x058A, video_class))
            self.assertFalse(video_nodes.node_matches(node, 3, [2], 0x058C, video_class))
            self.assertFalse(video_nodes.node_matches(node, 1, [2], 0x058A, video_class))

    def test_capture_modes_are_read_from_the_firmware(self):
        sizes = [(3448, 808), (898, 200)]
        rates = {(3448, 808): [(1, 60), (1, 30)], (898, 200): [(1, 240)]}

        def fake_ioctl(fd, request, argument):
            if request == video_nodes.VIDIOC_ENUM_FRAMESIZES:
                index, pixel_format, *_ = video_nodes._FRAME_SIZE.unpack(argument)
                if index >= len(sizes) or pixel_format != video_nodes.PIX_FMT_YUYV:
                    raise OSError(errno.EINVAL, 'end')
                return video_nodes._FRAME_SIZE.pack(index, pixel_format, video_nodes.FRMSIZE_DISCRETE, *sizes[index])
            index, pixel_format, width, height, *_ = video_nodes._FRAME_INTERVAL.unpack(argument)
            options = rates[(width, height)]
            if index >= len(options):
                raise OSError(errno.EINVAL, 'end')
            return video_nodes._FRAME_INTERVAL.pack(index, pixel_format, width, height,
                                                    video_nodes.FRMIVAL_DISCRETE, *options[index])

        with patch.object(video_nodes.os, 'open', return_value=99), patch.object(video_nodes.os, 'close'), \
                patch.object(video_nodes.fcntl, 'ioctl', fake_ioctl):
            modes = video_nodes.capture_modes('/dev/video0')
        self.assertEqual(modes, {(3448, 808): (30, 60), (898, 200): (240,)})

    def test_camera_controls_are_listed(self):
        table = [(0x00980900, 1, b'Brightness', 0, 255, 1, 128, 0, 140),
                 (0x00980913, 1, b'Gain', 0, 63, 1, 16, 0x0001, 0),        # disabled: skipped
                 (0x009a0902, 1, b'Exposure, Absolute', 3, 2047, 1, 250, 0, 500)]

        def fake_ioctl(fd, request, argument):
            if request == video_nodes.VIDIOC_QUERYCTRL:
                wanted = video_nodes._QUERY_CONTROL.unpack(argument)[0] & ~video_nodes.CTRL_FLAG_NEXT
                for entry in table:
                    if entry[0] > wanted:
                        return video_nodes._QUERY_CONTROL.pack(*entry[:8])
                raise OSError(errno.EINVAL, 'end')
            wanted = video_nodes._CONTROL.unpack(argument)[0]
            return video_nodes._CONTROL.pack(wanted, next(e[8] for e in table if e[0] == wanted))

        with patch.object(video_nodes.os, 'open', return_value=99), patch.object(video_nodes.os, 'close'), \
                patch.object(video_nodes.fcntl, 'ioctl', fake_ioctl):
            found = video_nodes.controls('/dev/video0')
        self.assertEqual([(c['name'], c['value'], c['maximum']) for c in found],
                         [('Brightness', 140, 255), ('Exposure, Absolute', 500, 2047)])

    def test_enum_ioctl_numbers_match_the_kernel(self):
        self.assertEqual(video_nodes.VIDIOC_ENUM_FRAMESIZES, 0xC02C564A)
        self.assertEqual(video_nodes.VIDIOC_ENUM_FRAMEINTERVALS, 0xC034564B)
        self.assertEqual((video_nodes.VIDIOC_QUERYCTRL, video_nodes.VIDIOC_G_CTRL), (0xC0445624, 0xC008561B))

    def test_querycap_ioctl_number(self):
        self.assertEqual(video_nodes.VIDIOC_QUERYCAP, 0x80685600)


class HotplugTests(unittest.TestCase):
    def test_video_and_usb_nodes_wake_the_watcher(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            watcher = DeviceWatcher(root)
            try:
                self.assertFalse(watcher.wait(0))
                (root / 'video3').touch()
                self.assertTrue(watcher.wait(1))
                (root / 'bus' / 'usb' / '003').mkdir(parents=True)
                self.assertTrue(watcher.wait(1))
                (root / 'bus' / 'usb' / '003' / '002').touch()
                self.assertTrue(watcher.wait(1))
                (root / 'random-file').touch()
                self.assertFalse(watcher.wait(0.2))
            finally:
                watcher.close()

    def test_tracker_debounces_a_burst_into_one_scan(self):
        scans = []

        class Clock:
            now = 0.0

            def __call__(self):
                return self.now

        clock = Clock()

        class Watcher:
            def __init__(self):
                self.calls = 0

            def wait(self, timeout=None):
                self.calls += 1
                if self.calls <= 3:
                    return True           # three nodes appear for one replug
                if self.calls == 4:
                    clock.now += timeout  # debounce expires
                    return False
                raise StopIteration

        with tempfile.TemporaryDirectory() as folder:
            tracker = camera_loader.CameraTracker('ps4', Watcher(), ModelMemory(Path(folder) / 'm.json'),
                                                  firmware_root=Path(folder), clock=clock,
                                                  scan=lambda: scans.append(1) or {})
            tracker.settle_index = len(camera_loader.SETTLE_CHECKS) + 1
            with patch.object(camera_loader, 'write_json'), patch.object(camera_loader, 'camera_nodes', dict):
                with self.assertRaises(StopIteration):
                    tracker.run()
        self.assertEqual(len(scans), 2)    # startup + one debounced rescan


if __name__ == '__main__':
    unittest.main()
