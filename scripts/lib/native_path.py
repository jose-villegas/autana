"""Native drive paths for tools launched from Git Bash; Linux paths pass through."""
import os
import re


def to_native(path):
    match = re.match(r'^/([A-Za-z])/(.*)$', path)
    if match and os.name == 'nt':
        return f'{match.group(1)}:/{match.group(2)}'
    return path
