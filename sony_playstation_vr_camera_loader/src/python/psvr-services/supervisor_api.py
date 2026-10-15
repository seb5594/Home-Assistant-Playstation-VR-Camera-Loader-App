"""The few Home Assistant Supervisor API calls the app makes.

Every app gets a token in SUPERVISOR_TOKEN and reaches the Supervisor as
http://supervisor. Calls bypass any HTTP proxy configured in the container.
"""
import json
import os
import urllib.error
import urllib.request

BASE_URL = 'http://supervisor'
TIMEOUT = 3


class SupervisorError(RuntimeError):
    pass


def call(method, path, body=None, timeout=TIMEOUT):
    """JSON request; returns the reply's "data" (or {}) and raises SupervisorError."""
    token = os.environ.get('SUPERVISOR_TOKEN')
    if not token:
        raise SupervisorError('Supervisor API token is unavailable')
    data = json.dumps(body).encode() if body is not None else None
    request = urllib.request.Request(BASE_URL + path, data=data, method=method,
                                     headers={'Authorization': f'Bearer {token}',
                                              'Content-Type': 'application/json'})
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        with opener.open(request, timeout=timeout) as response:
            reply = json.loads(response.read(1024 * 1024) or b'{}')
    except (OSError, ValueError, urllib.error.URLError) as exc:
        raise SupervisorError(f'{method} {path}: {exc}') from exc
    if not isinstance(reply, dict) or reply.get('result') != 'ok':
        message = reply.get('message') if isinstance(reply, dict) else None
        raise SupervisorError(f'{method} {path}: {message or "rejected"}')
    return reply.get('data') or {}


def reload_audio():
    """Ask Supervisor to refresh its cached audio device list (no audio restart)."""
    call('POST', '/audio/reload', {})


def app_options():
    """The app's current configuration, with defaults filled in."""
    return call('GET', '/addons/self/info').get('options') or {}


def app_slug():
    """Full slug of this installation, including the repository prefix (abcd1234_name)."""
    slug = call('GET', '/addons/self/info').get('slug')
    if not slug:
        raise SupervisorError('GET /addons/self/info: no slug')
    return slug


def update_camera_options(changes):
    """Write keys into the camera group of the app configuration, keeping everything else."""
    options = app_options()
    camera = dict(options.get('camera') or {})
    if all(camera.get(key) == value for key, value in changes.items()):
        return False
    camera.update(changes)
    options['camera'] = camera
    call('POST', '/addons/self/options', {'options': options})
    return True
