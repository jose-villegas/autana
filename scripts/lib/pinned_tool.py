"""Resolve the pinned clang-format and clang-tidy for the formatting and analysis gates.

clang-format is the release scripts/gates/requirements.txt pins, which pip installs
exactly. clang-tidy is the esp-clang the pinned ESP-IDF bundles (launcher/idf-pin.json),
the one CI's ESP-IDF container runs. The exact release is preferred; another patch
release of the same major runs with a note naming both; another major is refused.
"""
import argparse
import glob
import os
from pathlib import Path
import re
from shutil import which
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'launcher/tools/build'))
from espressif import IDF_PIN, espressif_tools_root, idf_pin

REQUIREMENTS = 'scripts/gates/requirements.txt'
OWNER = {'clang-format': REQUIREMENTS, 'clang-tidy': IDF_PIN}


def pinned_version(tool, root=Path(__file__).resolve().parents[2]):
    if tool == 'clang-tidy':
        return idf_pin(root)['esp_clang']
    for line in (root / REQUIREMENTS).read_text(encoding='utf-8').splitlines():
        name, _, version = line.partition('==')
        if name.strip() == tool:
            return version.strip()
    sys.exit(f'{REQUIREMENTS} pins no {tool}')


def tool_version(binary):
    try:
        result = subprocess.run([binary, '--version'], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r'version\s+(\d+(?:\.\d+)*)', result.stdout)
    return match.group(1) if result.returncode == 0 and match else None


def candidates(tool):
    major = pinned_version(tool).split('.')[0]
    names = [os.environ.get('CLANG_TIDY')] if tool == 'clang-tidy' else []
    names += [which(name) for name in (f'{tool}-{major}', tool)]
    found = []
    for suffix in ('', '.exe'):
        found.extend(glob.glob(str(espressif_tools_root() / 'tools/esp-clang/*/esp-clang/bin' / (tool + suffix))))
    key = lambda p: tuple(int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p))
    return [name for name in names if name] + sorted(found, key=key, reverse=True)


def resolve(tool):
    pinned = pinned_version(tool)
    pinned_major = pinned.split('.')[0]
    near = fallback = None
    for candidate in candidates(tool):
        version = tool_version(candidate)
        if version is None:
            continue
        major = version.split('.')[0]
        if version == pinned:
            return candidate, major
        if major == pinned_major and near is None:
            near = candidate, version
        if fallback is None:
            fallback = candidate, major
    if near:
        print(f'note: {tool} {near[1]} here, {pinned} pinned by {OWNER[tool]}; '
              'CI runs the pinned one (docs/tools/Framework-Versions.md).', file=sys.stderr)
        return near[0], pinned_major
    escape = tool.upper().replace('-', '_') + '_ANY_VERSION'
    if fallback and os.environ.get(escape) == '1':
        print(f'WARNING: using {tool} {fallback[1]}, not the pinned {pinned_major}.x.', file=sys.stderr)
        return fallback
    found = f'Found {tool} {fallback[1]} ({fallback[0]})' if fallback else f'No usable {tool} found'
    install = (f'pip install -r {REQUIREMENTS}' if tool == 'clang-format'
               else f'the ESP-IDF {IDF_PIN} pins, or LLVM {pinned_major}')
    sys.exit(f'{found}; this gate requires {tool} {pinned} ({OWNER[tool]}). '
             f'Install {install} (searched PATH and {espressif_tools_root()}/tools/esp-clang). '
             f'Set {escape}=1 for an informational run with another major.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', choices=('clang-format', 'clang-tidy'))
    parser.add_argument('--pinned', action='store_true', help='print the pinned release and exit')
    args = parser.parse_args()
    if args.pinned:
        print(pinned_version(args.tool))
        sys.exit(0)
    binary, major = resolve(args.tool)
    sys.stdout.write(major + '\n' + binary + '\n')
