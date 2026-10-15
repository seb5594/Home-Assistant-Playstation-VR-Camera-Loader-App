"""Shared setup for the Python tests: import path, temporary run dir, MQTT stub."""
import os
from pathlib import Path
import sys
import tempfile
import types

REPOSITORY = Path(__file__).resolve().parents[4]
ROOT = REPOSITORY / 'sony_playstation_vr_camera_loader'          # the app folder
SOURCE = ROOT / 'src' / 'python' / 'psvr-services'
ENGINE = ROOT / 'src' / 'c' / 'psvr-capture'
RUN_DIR = Path(tempfile.mkdtemp(prefix='psvr-test-run-'))
CONFIG_DIR = Path(tempfile.mkdtemp(prefix='psvr-test-config-'))
os.environ['PS_CAMERA_RUN_DIR'] = str(RUN_DIR)
os.environ['PS_CAMERA_CONFIG_DIR'] = str(CONFIG_DIR)
os.environ.setdefault('COLOR_LOGS', 'false')
os.environ.setdefault('LOG_LEVEL', 'error')
sys.path.insert(0, str(SOURCE))

# paho-mqtt is only installed in the app image.
if 'paho.mqtt.client' not in sys.modules:
    client = types.ModuleType('paho.mqtt.client')
    client.MQTT_ERR_SUCCESS = 0
    mqtt = types.ModuleType('paho.mqtt')
    mqtt.client = client
    paho = types.ModuleType('paho')
    paho.mqtt = mqtt
    sys.modules.update({'paho': paho, 'paho.mqtt': mqtt, 'paho.mqtt.client': client})


def make_usb_device(root, name, product_id, bus, devpath, address, configuration=1):
    """Create a sysfs USB device directory like /sys/bus/usb/devices/3-2."""
    entry = Path(root) / name
    entry.mkdir(parents=True)
    (entry / 'idVendor').write_text('05a9\n')
    (entry / 'idProduct').write_text(f'{product_id:04x}\n')
    (entry / 'busnum').write_text(f'{bus}\n')
    (entry / 'devpath').write_text(f'{devpath}\n')
    (entry / 'devnum').write_text(f'{address}\n')
    (entry / 'bConfigurationValue').write_text(f'{configuration}\n')
    return entry
