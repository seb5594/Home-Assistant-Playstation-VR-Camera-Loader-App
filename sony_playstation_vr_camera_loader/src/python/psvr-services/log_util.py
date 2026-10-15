"""Consistent colored service logs for the Home Assistant app."""
import os

COLORS = {'INFO': '36', 'WARNING': '33', 'ERROR': '31', 'SUCCESS': '32'}
RANK = {'DEBUG': 10, 'INFO': 20, 'SUCCESS': 20, 'WARNING': 30, 'ERROR': 40}


def log(level, message, scope=None):
    if RANK.get(level, 20) < RANK.get(os.environ.get('LOG_LEVEL', 'info').upper(), 20):
        return
    label = f'[{level}]'
    if os.environ.get('COLOR_LOGS', 'true').lower() == 'true' and level in COLORS:
        label = f'\033[{COLORS[level]}m{label}\033[0m'
    prefix = f' [{scope}]' if scope else ''
    print(f'{label}{prefix} {message}', flush=True)
