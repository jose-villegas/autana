"""Resolve the LLVM release shared by repository formatting and analysis gates.

The release is the esp-clang the pinned ESP-IDF bundles (launcher/idf-pin.json), so
CI and a developer with that ESP-IDF run the same binary's version. Another patch
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
from espressif import espressif_tools_root, idf_pin, pinned_tools_root

PINNED_VERSION = idf_pin()['esp_clang']
PINNED_MAJOR = PINNED_VERSION.split('.')[0]


def tool_version(binary):
    try:
        result = subprocess.run([binary, '--version'], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r'version\s+(\d+(?:\.\d+)*)', result.stdout)
    return match.group(1) if result.returncode == 0 and match else None


def tool_roots():
    """The tools installations the environment and the pin select."""
    return list(dict.fromkeys((espressif_tools_root(), pinned_tools_root())))


def candidates(tool, companion=None):
    siblings = []
    if companion:
        path = Path(companion)
        suffix = re.sub(r'^clang-tidy', '', path.name)
        siblings = [str(path.with_name(tool + suffix))]
        plain = str(path.with_name(tool + path.suffix))
        if plain not in siblings:
            siblings.append(plain)
    names = [os.environ.get('CLANG_TIDY')] if tool == 'clang-tidy' else []
    names += [which(name) for name in (f'{tool}-{PINNED_MAJOR}', tool)]
    found = []
    for root in tool_roots():
        for suffix in ('', '.exe'):
            found.extend(glob.glob(str(root / 'tools/esp-clang/*/esp-clang/bin' / (tool + suffix))))
    key = lambda p: tuple(int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p))
    return siblings + [name for name in names if name] + sorted(found, key=key, reverse=True)


def resolve(tool, companion=None):
    near = fallback = None
    for candidate in candidates(tool, companion):
        version = tool_version(candidate)
        if version is None:
            continue
        major = version.split('.')[0]
        if version == PINNED_VERSION:
            return candidate, major
        if major == PINNED_MAJOR and near is None:
            near = candidate, version
        if fallback is None:
            fallback = candidate, major
    if near:
        print(f'note: {tool} {near[1]} here, {PINNED_VERSION} pinned by launcher/idf-pin.json; '
              'CI runs the pinned one (docs/tools/Framework-Versions.md).', file=sys.stderr)
        return near[0], PINNED_MAJOR
    escape = tool.upper().replace('-', '_') + '_ANY_VERSION'
    if fallback and not companion and os.environ.get(escape) == '1':
        print(f'WARNING: using {tool} {fallback[1]}, not the pinned {PINNED_MAJOR}.x.', file=sys.stderr)
        return fallback
    searched = ', '.join(str(r / 'tools/esp-clang') for r in tool_roots())
    found = f'Found {tool} {fallback[1]} ({fallback[0]})' if fallback else f'No usable {tool} found'
    sys.exit(f'{found}; this gate requires {tool} {PINNED_MAJOR}.x. '
             f'Install LLVM {PINNED_MAJOR} or ESP-IDF (searched PATH and '
             f'{searched}). '
             + (f'Set {escape}=1 for an informational run with another major.' if not companion else ''))


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', choices=('clang-format', 'clang-tidy'))
    parser.add_argument('--pinned', action='store_true', help='print the pinned release and exit')
    args = parser.parse_args()
    if args.pinned:
        print(PINNED_VERSION)
        sys.exit(0)
    binary, major = resolve(args.tool)
    sys.stdout.write(major + '\n' + binary + '\n')
