"""Picture controls: brightness, colour, noise filter and friends.

Where the values live:

  app configuration   camera.picture_* options; set in the Configuration tab,
                      and written back when they are changed on the app page
  video_tuning.json   last values saved on the app page (in /config), so a
                      change is never lost while the configuration is updated
  <run dir>/video.json  what psvr-capture applies; re-read within a second

At start the app configuration wins; keys it does not have yet (older
installations) come from video_tuning.json, then from the defaults. Ranges
match src/c/psvr-capture/preview.c.
"""
import math
import os
from pathlib import Path

from runtime import CONFIG_DIR, RUN_DIR, locked, read_json, write_json

STORE = CONFIG_DIR / 'video_tuning.json'
LIVE = RUN_DIR / 'video.json'
OPTIONS = Path(os.environ.get('PS_CAMERA_OPTIONS', '/data/options.json'))

# name: (default, minimum, maximum); booleans are stored as 0/1 ranges
CONTROLS = {
    'brightness': (0, -100, 100),
    'contrast': (100, 50, 200),
    'gamma': (100, 50, 250),
    'saturation': (160, 0, 400),
    'auto_levels': (1, 0, 1),
    'white_balance': (1, 0, 1),
    'denoise': (1, 0, 2),
}
SWITCHES = ('auto_levels', 'white_balance')
NOISE_FILTER = ('off', 'light', 'strong')        # option values for denoise 0, 1, 2
OPTION_NAMES = {
    'brightness': 'picture_brightness',
    'contrast': 'picture_contrast',
    'gamma': 'picture_gamma',
    'saturation': 'picture_saturation',
    'auto_levels': 'picture_auto_levels',
    'white_balance': 'picture_white_balance',
    'denoise': 'picture_noise_filter',
}


def defaults():
    return {name: default for name, (default, _low, _high) in CONTROLS.items()}


def sanitize(values, base=None):
    """Known keys only, clamped to their range; anything missing comes from `base`."""
    result = dict(defaults() if base is None else base)
    for name, value in (values or {}).items():
        if name not in CONTROLS:
            continue
        _default, low, high = CONTROLS[name]
        try:
            number = float(value)
        except (TypeError, ValueError):
            continue
        if not math.isfinite(number):
            continue
        result[name] = max(low, min(high, int(round(number))))
    return result


# ---- app configuration format --------------------------------------------------

def from_options(camera):
    """Controls found in the camera options group (only the keys that are set)."""
    values = {}
    for name, option in OPTION_NAMES.items():
        value = (camera or {}).get(option)
        if value is None:
            continue
        if name == 'denoise':
            if value in NOISE_FILTER:
                values[name] = NOISE_FILTER.index(value)
        elif name in SWITCHES:
            values[name] = 1 if value is True or str(value).lower() == 'true' else 0
        else:
            values[name] = value
    return sanitize(values, {}) if values else {}


def to_options(values):
    """Camera option keys for `values` (booleans and named noise filter levels)."""
    options = {}
    for name, value in sanitize(values).items():
        if name == 'denoise':
            options[OPTION_NAMES[name]] = NOISE_FILTER[value]
        elif name in SWITCHES:
            options[OPTION_NAMES[name]] = bool(value)
        else:
            options[OPTION_NAMES[name]] = value
    return options


def configured(options_file=OPTIONS):
    """Controls set in the app configuration, as the Supervisor handed it to the app."""
    return from_options((read_json(options_file, {}) or {}).get('camera'))


def missing_in_configuration(options_file=OPTIONS):
    return set(CONTROLS) - set(configured(options_file))


# ---- load and save -------------------------------------------------------------

def load(store=STORE, options_file=OPTIONS):
    """Start values: app configuration, then the last page save, then the defaults."""
    values = sanitize(read_json(store, {}))
    values.update(configured(options_file))
    return values


def startup(store=STORE, live=LIVE, options_file=OPTIONS):
    """Apply the start values and remember them as the page's current state."""
    values = load(store, options_file)
    with locked(store):
        write_json(store, values)
        publish(values, live)
    return values


def current(store=STORE, options_file=OPTIONS):
    """What psvr-capture applies right now: the last save, else the start values."""
    return sanitize(read_json(store, {}), load(store, options_file))


def publish(values, live=LIVE):
    """Hand the controls to psvr-capture (JSON booleans for the switches)."""
    document = {name: bool(value) if name in SWITCHES else value for name, value in values.items()}
    write_json(live, document)


def save(changes, store=STORE, live=LIVE):
    """Apply changes from the app page right away; the caller updates the configuration."""
    with locked(store):            # two browser tabs may save at the same time
        values = sanitize(changes, current(store))
        write_json(store, values)
        publish(values, live)
    return values


def reset(store=STORE, live=LIVE):
    return save(defaults(), store, live)
