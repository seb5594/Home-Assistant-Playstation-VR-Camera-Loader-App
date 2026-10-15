#!/usr/bin/env python3
"""Mirror the microphone into a host ALSA loopback card, if the host provides one.

Only starts when snd-aloop was loaded by the host (the app never loads kernel
modules). Other containers can then record from hw:PSCamera,1,0.
"""
from pathlib import Path
import re
import signal
import subprocess
import time

from log_util import log as _log
from pulse_source import SAMPLE_RATE, SOURCE
from runtime import AUDIO_STATUS, RUN_DIR, read_json, write_json

CARD_ID = 'PSCamera'
STATUS = RUN_DIR / 'alsa.json'
running = True


def log(level, message):
    _log(level, message, 'ALSA')


def stop(*_args):
    global running
    running = False


def card_index(cards_text):
    match = re.search(r'^\s*(\d+)\s+\[' + CARD_ID + r'\s*\]:', cards_text, re.M)
    return int(match.group(1)) if match else None


def find_card():
    cards = Path('/proc/asound/cards')
    index = card_index(cards.read_text()) if cards.exists() else None
    if index is None:
        raise RuntimeError(f'no ALSA loopback card named {CARD_ID} on the host')
    if not Path(f'/dev/snd/pcmC{index}D0p').exists():
        raise RuntimeError(f'/dev/snd/pcmC{index}D0p is not mapped into the app')
    return index


def microphone_live():
    return read_json(AUDIO_STATUS, {}).get('state') == 'streaming'


def bridge(card):
    recording = subprocess.Popen(['parec', '--raw', '--format=s16le', f'--rate={SAMPLE_RATE}', '--channels=1',
                                  '--latency-msec=40', f'--device={SOURCE}'],
                                 stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    playback = subprocess.Popen(['aplay', '-q', '-D', f'hw:{CARD_ID},0,0', '-t', 'raw', '-f', 'S16_LE',
                                 '-r', str(SAMPLE_RATE), '-c', '1'],
                                stdin=recording.stdout, stderr=subprocess.PIPE)
    recording.stdout.close()
    log('INFO', f'Microphone mirrored to ALSA card {CARD_ID}; record with hw:{CARD_ID},1,0')
    write_json(STATUS, {'state': 'streaming', 'card': card, 'capture_device': f'hw:{CARD_ID},1,0'})
    try:
        while running and recording.poll() is None and playback.poll() is None and microphone_live():
            time.sleep(0.5)
    finally:
        for process in (recording, playback):
            if process.poll() is None:
                process.terminate()
        for process in (recording, playback):
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
        detail = playback.stderr.read(300).decode(errors='replace').strip()
        playback.stderr.close()
        if running:
            log('WARNING', f'ALSA mirror stopped; restarting. {detail}'.strip())


def main():
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        card = find_card()
    except (OSError, RuntimeError) as exc:
        log('INFO', f'Host ALSA mirror unavailable: {exc}')
        write_json(STATUS, {'state': 'unavailable', 'error': str(exc)})
        return
    while running:
        try:
            if microphone_live():
                bridge(card)
        except (OSError, subprocess.SubprocessError) as exc:
            log('WARNING', f'ALSA mirror: {exc}')
        time.sleep(2)
    write_json(STATUS, {'state': 'stopped', 'card': card})


if __name__ == '__main__':
    main()
