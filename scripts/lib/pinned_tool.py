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
    try:
        result = subprocess.run([binary, '--version'], capture_output=True, text=True, timeout=15)
    except OSError:
        return None
    match = re.search(r'version\s+(\d+)', result.stdout)
    return match.group(1) if match else None


def candidates(tool):
    names = []
    if tool == 'clang-tidy' and os.environ.get('CLANG_TIDY'):
        names.append(os.environ['CLANG_TIDY'])
    for name in (f'{tool}-{PINNED_MAJOR}', tool):
        found = which(name)
        if found:
            names.append(name if tool == "clang-format" else found)
    suffixes = ('.exe',) if os.name == 'nt' else ('',)
    if tool == 'clang-format':
        suffixes = ('', '.exe')
    found = []
    for suffix in suffixes:
        found.extend(glob.glob(str(espressif_tools_root() / 'tools/esp-clang/*/esp-clang/bin' / (tool + suffix))))
    key = (lambda p: tuple(int(x) if x.isdigit() else x for x in re.split(r'(\d+)', p))) if tool == 'clang-format' else None
    return names + sorted(found, key=key, reverse=True)


def format_install_help():
    lines = ["", f"This repository formats with clang-format {PINNED_MAJOR}.x and no other major version.",
             "", "Get it:", f"  any platform:  pip install 'clang-format=={PINNED_MAJOR}.1.7'",
             "  ESP-IDF:       already bundled - source the IDF export script, or let this",
             "                 script find $IDF_TOOLS_PATH (default ~/.espressif)",
             "                 /tools/esp-clang/*/esp-clang/bin/"]
    if sys.platform == "linux":
        lines += [f"  Linux:         apt-get install clang-format-{PINNED_MAJOR}",
                  "                 (from the LLVM apt repository if your distribution is older)"]
    else:
        lines.append("  Other:         https://releases.llvm.org/")
    lines += ["", "To run anyway with a different major (it may reformat files you did not touch):",
              f"  CLANG_FORMAT_ANY_VERSION=1 {os.environ.get('CLANG_FORMAT_GATE', 'scripts/gates/check-format.sh')} ..."]
    print("\n".join(lines), file=sys.stderr)


def resolve(tool):
    fallback = None
    for candidate in candidates(tool):
        major = tool_major(candidate)
        if major is None:
            continue
        if major == PINNED_MAJOR:
            return candidate, major
        if fallback is None:
            fallback = candidate, major
    if tool == "clang-format":
        if fallback is None:
            print(f"No clang-format found on PATH or in {espressif_tools_root()}/tools/esp-clang/.", file=sys.stderr)
        elif os.environ.get("CLANG_FORMAT_ANY_VERSION") == "1":
            candidate, major = fallback
            print(f"WARNING: using clang-format {major}, not the pinned {PINNED_MAJOR}.x.", file=sys.stderr)
            print(f"WARNING: it may reformat files you did not touch. CI checks with {PINNED_MAJOR}.x.", file=sys.stderr)
            return fallback
        else:
            candidate, major = fallback
            print(f"Found clang-format {major} ({candidate}), but this repository pins {PINNED_MAJOR}.x.", file=sys.stderr)
        format_install_help()
        sys.exit(1)
    if fallback is None:
        sys.exit(
            "No clang-tidy found: not on PATH, not in $CLANG_TIDY, and not "
            f"under {espressif_tools_root() / 'tools' / 'esp-clang'}. This gate needs clang-tidy "
            f"{PINNED_MAJOR}.x - ESP-IDF's bundled esp-clang carries it "
            "(install ESP-IDF, or source its export script so PATH finds "
            "it), or install LLVM 19 directly "
            f"(apt install clang-tidy-{PINNED_MAJOR})."
        )
    candidate, major = fallback
    if os.environ.get("CLANG_TIDY_ANY_VERSION") != "1":
        sys.exit(
            f"Found clang-tidy {major} ({candidate}), but this gate pins "
            f"{PINNED_MAJOR}.x, the same major scripts/gates/check-format.sh pins "
            "clang-format to and for the same reason: different majors score "
            "this check differently. Set CLANG_TIDY_ANY_VERSION=1 to run "
            "anyway (informational only - CI always uses the pinned major)."
        )
    print(f"WARNING: using clang-tidy {major}, not the pinned {PINNED_MAJOR}.x.",
          file=sys.stderr)
    return candidate, major


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('tool', choices=('clang-format', 'clang-tidy'))
    args = parser.parse_args()
    binary, major = resolve(args.tool)
    print(major)
    print(binary)
