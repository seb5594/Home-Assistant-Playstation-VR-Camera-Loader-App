#!/usr/bin/env python3
"""Publish the camera preview as a Home Assistant camera via MQTT discovery."""
import argparse
import json
import os
import signal
import time

import paho.mqtt.client as mqtt

from log_util import log
from runtime import CONFIG_DIR, SNAPSHOT, file_age, request_preview

DISCOVERY = 'homeassistant/camera/sony_playstation_camera/config'
IMAGE = 'sony_playstation_camera/image'
AVAILABILITY = 'sony_playstation_camera/availability'
DISCOVERY_MARKER = CONFIG_DIR / 'mqtt_discovery_active'
STALE_SECONDS = 10

running = True


def stop(*_args):
    global running
    running = False


def discovery_payload():
    return json.dumps({
        'name': 'PlayStation Camera',
        'unique_id': 'sony_playstation_camera',
        'topic': IMAGE,
        'availability_topic': AVAILABILITY,
        'device': {'identifiers': ['sony_playstation_camera'], 'manufacturer': 'Sony', 'name': 'PlayStation Camera'},
    })


def publish_latest(client, previous):
    """Send the newest still once; mark the entity offline when images stop."""
    request_preview()
    age = file_age(SNAPSHOT)
    if age is None or age > STALE_SECONDS:
        client.publish(AVAILABILITY, 'offline', qos=1, retain=True)
        return previous
    modified = SNAPSHOT.stat().st_mtime_ns
    if modified == previous:
        return previous
    frame = SNAPSHOT.read_bytes()
    if not frame.startswith(b'\xff\xd8'):
        return previous
    if client.publish(IMAGE, frame, qos=0, retain=False).rc == mqtt.MQTT_ERR_SUCCESS:
        client.publish(AVAILABILITY, 'online', qos=1, retain=True)
        return modified
    return previous


def make_client():
    client = mqtt.Client(client_id='sony-playstation-camera')
    if os.environ.get('MQTT_USERNAME'):
        client.username_pw_set(os.environ['MQTT_USERNAME'], os.environ.get('MQTT_PASSWORD', ''))
    client.will_set(AVAILABILITY, 'offline', qos=1, retain=True)
    return client


def remove_discovery(client):
    client.publish(DISCOVERY, b'', qos=1, retain=True).wait_for_publish(timeout=5)
    client.publish(AVAILABILITY, 'offline', qos=1, retain=True)
    DISCOVERY_MARKER.unlink(missing_ok=True)
    log('INFO', 'Removed the MQTT camera entity')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--remove', action='store_true', help='delete the discovery entry and exit')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    host, port = os.environ['MQTT_HOST'], int(os.environ.get('MQTT_PORT', '1883'))

    while running:
        client = make_client()
        try:
            client.connect(host, port, keepalive=30)
            client.loop_start()
            if args.remove:
                remove_discovery(client)
                return
            client.publish(DISCOVERY, discovery_payload(), qos=1, retain=True).wait_for_publish(timeout=5)
            DISCOVERY_MARKER.write_text(host, encoding='utf-8')
            log('INFO', 'Publishing the camera entity through MQTT discovery')
            previous = None
            while running:
                try:
                    previous = publish_latest(client, previous)
                except OSError:
                    pass  # the still is replaced atomically; a race just skips one tick
                time.sleep(1)
        except (OSError, ValueError) as exc:
            log('WARNING', f'MQTT broker {host}:{port} unavailable: {exc}; retrying in 10 s')
            if args.remove:
                return
            time.sleep(10)
        finally:
            if running is False and not args.remove:
                try:
                    client.publish(AVAILABILITY, 'offline', qos=1, retain=True).wait_for_publish(timeout=2)
                except (OSError, ValueError, RuntimeError):
                    pass
            client.loop_stop()
            client.disconnect()


if __name__ == '__main__':
    main()
