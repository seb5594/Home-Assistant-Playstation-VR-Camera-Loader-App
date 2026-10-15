"""Expose the processed camera audio as a microphone in Home Assistant's PulseAudio.

Apps cannot create files inside the audio container (/run/audio is mounted
read-only), so a pipe source is impossible. The trick that works: a null
sink receives our PCM, and its monitor is remapped into a proper input
source with a friendly name. The null sink runs at 16 kHz mono, the rate
every wake word and speech-to-text engine asks for, so no resampling happens
on the way to Assist.
"""
import subprocess

from log_util import log as _log
from supervisor_api import SupervisorError, reload_audio

SAMPLE_RATE = 16000
SINK = 'sony_playstation_vr_camera_output'
MONITOR = SINK + '.monitor'
SOURCE = 'sony_playstation_vr_camera_input'
# Takes the place of the default output when the host has no speaker at all;
# otherwise PulseAudio would make our transport the default and feed Home
# Assistant's own voice replies straight back into the microphone.
SILENCE_SINK = 'sony_playstation_vr_camera_silence'
MICROPHONE_LABEL = 'USB Sony PlayStation VR Camera Microphone'
TRANSPORT_LABEL = 'Sony PlayStation VR Camera internal microphone transport (no speaker)'
SILENCE_LABEL = 'No speaker connected (sound is discarded)'

# Names registered by older releases; removed when found with their exact arguments.
LEGACY_REMAPS = {'alsa_input.playstation_camera': {'playstation_camera_input', SOURCE},
                 'playstation_camera_input': {'playstation_camera_microphone.monitor'}}
LEGACY_SINK = 'playstation_camera_microphone'


def log(level, message):
    _log(level, message, 'Audio')


def pactl(command, *args):
    result = subprocess.run(['pactl', command, *args], capture_output=True, text=True, timeout=5)
    if result.returncode:
        raise RuntimeError(f'pactl {command}: {result.stderr.strip()}')
    return result.stdout.strip()


def _names(listing):
    return {line.split('\t')[1] for line in listing.splitlines() if line.count('\t') >= 1}


def _modules():
    """[(id, name, argument tokens)] of loaded PulseAudio modules."""
    modules = []
    for line in pactl('list', 'short', 'modules').splitlines():
        fields = line.split('\t', 2)
        if len(fields) == 3:
            modules.append((fields[0], fields[1], fields[2].split()))
    return modules


def _load_named(module, *arguments, description):
    """Load with a display name; fall back for servers that reject quoted properties."""
    key = 'sink_properties' if module == 'module-null-sink' else 'source_properties'
    attempts = (f"{key}=device.description='{description.replace(' ', chr(92) + ' ')}'",
                f"{key}=device.description={description.replace(' ', '_')}", None)
    last_error = None
    for prop in attempts:
        try:
            module_id = pactl('load-module', module, *arguments, *([prop] if prop else []))
            if module_id.isdigit():
                return module_id
            last_error = RuntimeError(f'unexpected module id {module_id!r}')
        except RuntimeError as exc:
            last_error = exc
    raise last_error


class PulseMicrophone:
    """Owns the null sink + remapped source while a camera is connected."""

    def __init__(self, set_default_input=False):
        self.set_default_input = set_default_input
        self.modules = []
        self.source = SOURCE
        self.previous_default = None
        self.default_active = False

    @property
    def ready(self):
        return bool(self.modules) or self.source == MONITOR

    def remove_stale(self):
        """Unload modules left by an earlier run (crash, upgrade, 48 kHz sink)."""
        removed = 0
        for module_id, name, args in reversed(_modules()):
            ours = ((name == 'module-remap-source' and f'source_name={SOURCE}' in args) or
                    (name == 'module-null-sink' and (f'sink_name={SINK}' in args or
                                                     f'sink_name={SILENCE_SINK}' in args)))
            legacy = name == 'module-remap-source' and any(
                f'source_name={old}' in args and any(f'master={master}' in args for master in masters)
                for old, masters in LEGACY_REMAPS.items())
            legacy |= name == 'module-null-sink' and f'sink_name={LEGACY_SINK}' in args
            if ours or legacy:
                try:
                    pactl('unload-module', module_id)
                    removed += 1
                except RuntimeError as exc:
                    log('WARNING', f'Could not remove old audio module {module_id}: {exc}')
        if removed:
            log('INFO', f'Removed {removed} audio module(s) from an earlier run')
        return removed

    def register(self):
        self.remove_stale()
        defaults_before = self._defaults()
        try:
            self.modules.append(_load_named('module-null-sink', f'sink_name={SINK}', f'rate={SAMPLE_RATE}',
                                            'channels=1', 'format=s16le', description=TRANSPORT_LABEL))
            if MONITOR not in _names(pactl('list', 'short', 'sources')):
                raise RuntimeError(f'PulseAudio created {SINK} but not its monitor')
        except RuntimeError:
            self.unregister()          # nothing half-made may stay behind
            raise
        try:
            self.modules.append(_load_named('module-remap-source', f'master={MONITOR}',
                                            f'source_name={SOURCE}', description=MICROPHONE_LABEL))
            self.source = SOURCE
        except RuntimeError as exc:
            log('WARNING', f'Named microphone input unavailable ({exc}); using the monitor source')
            self.source = MONITOR
        # From here on the microphone exists; default handling must never undo that.
        self._protect_defaults(*defaults_before)
        self._apply_default()
        log('INFO', f'Microphone available in Home Assistant as {self.source} ({SAMPLE_RATE} Hz mono)')
        self._refresh_supervisor()

    @staticmethod
    def _defaults():
        try:
            return pactl('get-default-sink'), pactl('get-default-source')
        except RuntimeError:
            return None, None

    def _protect_defaults(self, sink_before, source_before):
        """Undo automatic switching (module-switch-on-connect, module-always-sink) to our devices.

        Without this, Home Assistant's own speech and music can end up in the
        camera transport: inaudible, and fed straight back into the microphone.
        """
        sink_now, source_now = self._defaults()
        if sink_now in (SINK, LEGACY_SINK):
            try:
                sinks = _names(pactl('list', 'short', 'sinks'))
                usable = sinks - {SINK, LEGACY_SINK, SILENCE_SINK}
                fallback = sink_before if sink_before in usable else first_speaker()
                if fallback:
                    pactl('set-default-sink', fallback)
                    log('INFO', f'Default audio output was switched to the camera transport; restored {fallback}')
                else:
                    self.modules.append(_load_named('module-null-sink', f'sink_name={SILENCE_SINK}',
                                                    description=SILENCE_LABEL))
                    pactl('set-default-sink', SILENCE_SINK)
                    log('WARNING', 'No speaker found; sound for the default output is discarded instead of '
                        'landing in the camera microphone')
            except RuntimeError as exc:
                log('WARNING', f'Could not move the default audio output away from the camera transport: {exc}')
        if (not self.set_default_input and source_now in (SOURCE, MONITOR)
                and source_before and source_before not in (SOURCE, MONITOR)):
            try:
                pactl('set-default-source', source_before)
            except RuntimeError as exc:
                log('WARNING', f'Could not restore the default input {source_before}: {exc}')

    def _apply_default(self):
        try:
            current = pactl('get-default-source')
        except RuntimeError:
            return
        self.default_active = current == self.source
        if not self.set_default_input or self.default_active:
            return
        try:
            self.previous_default = current
            pactl('set-default-source', self.source)
            self.default_active = pactl('get-default-source') == self.source
        except RuntimeError as exc:
            log('WARNING', f'Could not make the camera the default input: {exc}')

    def unregister(self):
        if self.previous_default and self.default_active:
            try:
                if pactl('get-default-source') == self.source:
                    pactl('set-default-source', self.previous_default)
            except RuntimeError as exc:
                log('WARNING', f'Could not restore the previous default input: {exc}')
        had_modules = bool(self.modules)
        for module_id in reversed(self.modules):
            try:
                pactl('unload-module', module_id)
            except RuntimeError as exc:
                log('WARNING', f'Could not remove audio module {module_id}: {exc}')
        self.modules.clear()
        self.previous_default = None
        self.default_active = False
        self.source = SOURCE
        if had_modules:
            log('INFO', 'Microphone removed from Home Assistant')
            self._refresh_supervisor()

    @staticmethod
    def _refresh_supervisor():
        try:
            reload_audio()
        except SupervisorError as exc:
            log('WARNING', f'Home Assistant audio list not refreshed: {exc}')


def playback_command(client_name=MICROPHONE_LABEL):
    """pacat feeding the null sink from psvr-capture's stdout."""
    return ['pacat', '--playback', '--raw', '--format=s16le', f'--rate={SAMPLE_RATE}', '--channels=1',
            f'--device={SINK}', '--latency-msec=30', f'--client-name={client_name}']


class EchoReferenceError(RuntimeError):
    """Echo cancellation cannot start; `code` lets the web page show a translated hint."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code


def first_speaker():
    """First real playback device, used when the default points at our transport."""
    for name in sorted(_names(pactl('list', 'short', 'sinks'))):
        if name not in (SINK, LEGACY_SINK, SILENCE_SINK) and name.startswith('alsa_output.'):
            return name
    return None


def resolve_echo_reference(setting):
    """Sink whose monitor feeds the echo canceller, or None when disabled."""
    setting = (setting or '').strip()
    if not setting or setting.lower() in ('off', 'none', 'false'):
        return None
    if setting.lower() in ('default', 'auto'):
        sink = pactl('get-default-sink')
        # The configured default may be ours, the silence stand-in, or a device that is gone.
        if sink in (SINK, LEGACY_SINK, SILENCE_SINK) or sink not in _names(pactl('list', 'short', 'sinks')):
            sink = first_speaker()
            if sink is None:
                raise EchoReferenceError('no_speaker', 'no speaker found besides the camera transport')
    else:
        sink = setting
    if sink in (SINK, LEGACY_SINK):
        raise EchoReferenceError('transport', 'the camera transport is not a speaker; '
                                              'enter the output Assist and music play on')
    if sink not in _names(pactl('list', 'short', 'sinks')):
        raise EchoReferenceError('not_found', f'audio output {sink!r} not found (see "ha audio info")')
    return sink


def reference_command(sink):
    return ['parec', '--raw', '--format=s16le', f'--rate={SAMPLE_RATE}', '--channels=1',
            f'--device={sink}.monitor', '--latency-msec=20', '--client-name=PlayStation camera echo reference']
