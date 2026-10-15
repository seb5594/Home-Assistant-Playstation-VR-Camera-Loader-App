#!/usr/bin/env python3
"""Feed synthetic OV580 frames through psvr-capture and check PCM, status and JPEG.

Needs numpy (development only). Usage: tests/end_to_end.py ./psvr-capture
"""
import json
from pathlib import Path
import resource
import subprocess
import sys
import tempfile
import time

import numpy as np

WIDTH, HEIGHT, FPS = 3448, 808, 30
IMAGE_ROWS, SAMPLES_PER_ROW = 800, 2          # 48 kHz packed into 1280x800/30 fps rows
SECONDS = 8
TONE_HZ, TONE_LEVEL = 1000.0, 3000.0


def frames():
    """Yield raw frames: 1 kHz tone in four slightly delayed capsules, grey ramp image."""
    template = np.zeros((HEIGHT, WIDTH * 2), dtype=np.uint8)
    template[:, 96:96 + 2560:2] = np.linspace(16, 235, 1280, dtype=np.uint8)  # luma ramp, left eye
    template[:, 97:96 + 2560:2] = 128                                         # neutral chroma
    template[:, 1] = template[:, 2] = 0x22                                    # row markers
    rng = np.random.default_rng(1)
    position = 0
    for _ in range(SECONDS * FPS):
        frame = template.copy()
        count = IMAGE_ROWS * SAMPLES_PER_ROW
        t = (position + np.arange(count)) / 48000.0
        audio = np.zeros((IMAGE_ROWS, 4, 8), dtype=np.int16)
        for mic in range(4):
            wave = TONE_LEVEL * np.sin(2 * np.pi * TONE_HZ * (t - mic * 20e-6)) + rng.normal(0, 30, count)
            audio[:, mic, :SAMPLES_PER_ROW] = np.round(wave).astype(np.int16).reshape(IMAGE_ROWS, SAMPLES_PER_ROW)
        frame[:IMAGE_ROWS, 32:96] = audio.reshape(IMAGE_ROWS, 32).view(np.uint8)
        position += count
        yield frame.tobytes()


def main():
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else './psvr-capture').resolve()
    with tempfile.TemporaryDirectory() as run_dir:
        (Path(run_dir) / 'preview.request').touch()
        pcm_path = Path(run_dir) / 'out.pcm'
        with open(pcm_path, 'wb') as pcm:
            process = subprocess.Popen(
                # 'sum': the adaptive beamformer would rightly notch a never-ending tone.
                [str(binary), '--device', '-', '--file', '--audio', '--beamformer', 'sum',
                 '--preview', 'sd', '--preview-fps', '5', '--run-dir', run_dir],
                stdin=subprocess.PIPE, stdout=pcm)
            started = time.monotonic()
            for frame in frames():
                process.stdin.write(frame)
            process.stdin.close()
            code = process.wait(timeout=60)
            wall = time.monotonic() - started
        usage = resource.getrusage(resource.RUSAGE_CHILDREN)
        assert code == 0, f'engine exited with {code}'

        samples = np.frombuffer(pcm_path.read_bytes(), dtype=np.int16).astype(np.float64)
        expected = SECONDS * 16000
        assert abs(len(samples) - expected) < 2000, f'{len(samples)} samples, expected about {expected}'
        steady = samples[16000:len(samples) - 4000]
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(len(steady))))
        peak_hz = np.argmax(spectrum) * 16000 / len(steady)
        level = np.sqrt(np.mean(steady ** 2)) * np.sqrt(2)
        fit = np.vstack([np.sin(2 * np.pi * peak_hz * np.arange(len(steady)) / 16000),
                         np.cos(2 * np.pi * peak_hz * np.arange(len(steady)) / 16000)]).T
        residual = steady - fit @ np.linalg.lstsq(fit, steady, rcond=None)[0]
        snr = 10 * np.log10(np.mean(steady ** 2) / np.mean(residual ** 2))
        assert abs(peak_hz - TONE_HZ) < 2, f'tone at {peak_hz:.1f} Hz (pitch error)'
        assert 0.7 * TONE_LEVEL < level < 1.1 * TONE_LEVEL, f'tone amplitude {level:.0f}'
        assert snr > 30, f'tone SNR {snr:.1f} dB'

        status = json.loads((Path(run_dir) / 'engine.json').read_text())
        audio = status['audio']
        assert 'packed' in audio['layout'], audio['layout']
        assert abs(audio['input_rate'] - 48000) < 50, audio['input_rate']
        jpeg = (Path(run_dir) / 'still.jpg').read_bytes()
        assert jpeg.startswith(b'\xff\xd8') and jpeg.endswith(b'\xff\xd9'), 'preview JPEG missing'
        assert status['preview']['size'] == '640x400', status['preview']

        cpu = usage.ru_utime + usage.ru_stime
        print(f'  ok   end-to-end: {peak_hz:.1f} Hz tone, amplitude {level:.0f}, SNR {snr:.1f} dB, '
              f'layout "{audio["layout"]}", input {audio["input_rate"]:.0f} Hz')
        print(f'       engine CPU {cpu:.2f} s for {SECONDS} s of 1280x800@30 audio+preview '
              f'({100 * cpu / SECONDS:.1f} % of one core, wall {wall:.1f} s incl. frame synthesis)')


def window_test(interleaved, step, quiet, label):
    """PS4 at 60 fps: every row (trailer rows included) holds a window of the last 8
    values per capsule that moves on by `step` samples per row."""
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else './psvr-capture').resolve()
    fps, seconds = 60, 4
    rate = HEIGHT * fps * step
    total = fps * seconds * HEIGHT * step + 8
    rng = np.random.default_rng(2)
    t = np.arange(total) / rate
    capsules = np.stack([TONE_LEVEL * np.sin(2 * np.pi * TONE_HZ * (t - mic * 20e-6)) + rng.normal(0, 30, total)
                         for mic in range(4)], axis=1)
    if quiet:   # a silent room: every third 20 ms stays below the parser's equality threshold
        silent = (np.arange(total) // (rate // 50)) % 3 == 0
        capsules[silent] = rng.integers(-3, 4, (silent.sum(), 4))
    capsules = capsules.round().astype(np.int16)                                      # (samples, 4)
    template = np.zeros((HEIGHT, WIDTH * 2), dtype=np.uint8)
    template[:, 1] = template[:, 2] = 0x22
    window = np.arange(8)[None, :] + step * np.arange(HEIGHT)[:, None]                # oldest .. newest
    with tempfile.TemporaryDirectory() as run_dir:
        pcm_path = Path(run_dir) / 'out.pcm'
        with open(pcm_path, 'wb') as pcm:
            process = subprocess.Popen(
                [str(binary), '--device', '-', '--file', '--fps', str(fps), '--audio', '--beamformer', 'sum',
                 '--run-dir', run_dir], stdin=subprocess.PIPE, stdout=pcm)
            for index in range(fps * seconds):
                frame = template.copy()
                rows = capsules[index * HEIGHT * step + window]                          # (rows, 8, 4)
                if not interleaved:
                    rows = rows.transpose(0, 2, 1)                                          # capsule by capsule
                frame[:, 32:96] = np.ascontiguousarray(rows).reshape(HEIGHT, 32).view(np.uint8)
                process.stdin.write(frame.tobytes())
            process.stdin.close()
            assert process.wait(timeout=60) == 0
        samples = np.frombuffer(pcm_path.read_bytes(), dtype=np.int16).astype(np.float64)
        steady = samples[16000:len(samples) - 4000]
        spectrum = np.abs(np.fft.rfft(steady * np.hanning(len(steady))))
        peak_hz = np.argmax(spectrum) * 16000 / len(steady)
        audio = json.loads((Path(run_dir) / 'engine.json').read_text())['audio']
        assert f'{step} new' in audio['layout'] and (('interleaved' in audio['layout']) == interleaved), audio['layout']
        assert abs(audio['input_rate'] - rate) < rate * 0.002, audio['input_rate']
        assert abs(peak_hz - TONE_HZ) < 2, f'tone at {peak_hz:.1f} Hz'
        print(f'  ok   {label}: layout "{audio["layout"]}", input {audio["input_rate"]:.0f} Hz, tone {peak_hz:.1f} Hz')


def full_chain_smoke_test(beamformer):
    """Every stage on, plus the before/after recording and the raw row dump."""
    binary = Path(sys.argv[1] if len(sys.argv) > 1 else './psvr-capture').resolve()
    with tempfile.TemporaryDirectory() as run_dir:
        for name in ('compare', 'rows'):
            (Path(run_dir) / f'{name}.request').touch()
        process = subprocess.Popen(
            [str(binary), '--device', '-', '--file', '--audio', '--beamformer', beamformer,
             '--noise-suppression', 'rnnoise', '--auto-gain', '--run-dir', run_dir],
            stdin=subprocess.PIPE, stdout=subprocess.DEVNULL)
        for frame in frames():
            process.stdin.write(frame)
        process.stdin.close()
        assert process.wait(timeout=60) == 0
        for name in ('compare_raw.wav', 'compare_processed.wav'):
            data = (Path(run_dir) / name).read_bytes()
            assert data[:4] == b'RIFF' and len(data) == 44 + 4 * 16000 * 2, (name, len(data))
        rows = (Path(run_dir) / 'rows.bin').read_bytes()
        assert rows[:8] == b'PSVRROW1' and len(rows) == 8 + 16 + 60 * (8 + HEIGHT * 96), len(rows)
        assert not (Path(run_dir) / 'compare.request').exists()
        status = json.loads((Path(run_dir) / 'engine.json').read_text())['audio']
        assert status['beamformer'] == beamformer and 'presence' in status['focus'], status
        print(f'  ok   full chain ({beamformer} + RNNoise + auto gain), A/B recording and row dump')


if __name__ == '__main__':
    main()
    window_test(True, 1, False, 'interleaved window, 1 new frame per row')
    window_test(False, 4, True, 'capsule window, 4 new samples per row, quiet stretches (alpha15 hardware)')
    full_chain_smoke_test('adaptive')
    full_chain_smoke_test('focus')
