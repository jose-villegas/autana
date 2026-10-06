"""Resolve the LLVM major shared by repository formatting and analysis gates."""
import argparse
import glob
import os
from pathlib import Path
import re
from shutil import which
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'launcher/tools/build'))
from espressif import espressif_tools_root

PINNED_MAJOR = '19'


def tool_major(binary):
    """Read a tool version major, returning None when probing fails."""
    try:
        result = subprocess.run([binary, '--version'], capture_output=True, text=True, timeout=15)
    except (OSError, subprocess.TimeoutExpired):
        return None
    match = re.search(r'version\s+(\d+)', result.stdout)
    return match.group(1) if result.returncode == 0 and match else None


def candidates(tool):
    """Find tool candidates from the environment, PATH, and installed ESP toolchains."""
    names = [os.environ.get('CLANG_TIDY')] if tool == 'clang-tidy' else []
    names += [which(name) for name in (f'{tool}-{PINNED_MAJOR}', tool)]
    found = []
    for suffix in ('', '.exe'):
        found.extend(glob.glob(str(espressif_tools_root() / 'tools/esp-clang/*/esp-clang/bin' / (tool + suffix))))
    key = lambda p: tuple(int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p))
    return [name for name in names if name] + sorted(found, key=key, reverse=True)


def resolve(tool):
    """Resolve the pinned tool major or exit with installation guidance."""
    fallback = None
    for candidate in candidates(tool):
        major = tool_major(candidate)
        if major is None:
            continue
        if major == PINNED_MAJOR:
            return candidate, major
        if fallback is None:
            fallback = candidate, major
    escape = tool.upper().replace('-', '_') + '_ANY_VERSION'
    if fallback and os.environ.get(escape) == '1':
        print(f'WARNING: using {tool} {fallback[1]}, not the pinned {PINNED_MAJOR}.x.', file=sys.stderr)
        return fallback
    found = f'Found {tool} {fallback[1]} ({fallback[0]})' if fallback else f'No usable {tool} found'
    sys.exit(f'{found}; this gate requires {tool} {PINNED_MAJOR}.x. '
             f'Install LLVM {PINNED_MAJOR} or ESP-IDF (searched PATH and '
             f'{espressif_tools_root()}/tools/esp-clang). '
             f'Set {escape}=1 for an informational run with another major.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', choices=('clang-format', 'clang-tidy'))
    args = parser.parse_args()
    binary, major = resolve(args.tool)
    sys.stdout.write(major + '\n' + binary + '\n')
