#!/usr/bin/env python3
"""Require logging or a source note for soft-double calls in firmware.

    python launcher/tools/build/check_no_soft_double.py [build-dir]

The ESP32-S3's FPU is single precision only, so every `double` operation is a
libgcc call (__adddf3, __muldf3, __divdf3, __extendsfdf2, __truncdfsf2, ...)
at about ten times the cost of the float one. -Werror=double-promotion and
-Werror=float-conversion (launcher/main/CMakeLists.txt) stop the implicit
ones at compile time; this reads first-party objects and
finds the explicit ones the compiler accepts, such as a `(double)` cast or a
double constant, by the function that calls them.

A function that calls a logging or formatting routine (esp_log, printf,
snprintf, ...) is exempt because a `%f` argument is a double. A deliberate
soft-double call elsewhere needs a `Library call: <reason>` note covering
its source line, using the same convention as the hot-call link gate.

The build directory defaults to launcher/build.diag; any build of the
firmware works. Needs the Xtensa objdump from ESP-IDF's toolchain.
"""

import pathlib
import re
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import packages  # noqa: E402
from check_hot_calls import annotations, code_layout, own_source, source_site  # noqa: E402
from espressif import xtensa_tool  # noqa: E402
from objdump_calls import RELOCATION, disassembly_calls, parse_calls  # noqa: E402,F401

SOFT_DOUBLE = re.compile(r"^__\w*df\w*$")
LOGGING = re.compile(r"^(esp_log|esp_log_write|esp_log_writev|printf|snprintf|vsnprintf|sprintf|esp_rom_printf|"
                     r"ets_printf|fprintf|vprintf|__wrap_\w*printf)$")


def objects(build, launcher=packages.LAUNCHER):
    """The first-party objects: the main component's and each package's (packages.py)."""
    components = ["main", *(include.parent.name for include in packages.include_dirs(launcher))]
    roots = [build / "esp-idf" / name / "CMakeFiles" / f"__idf_{name}.dir" for name in components]
    found = sorted(path for root in roots for path in [*root.rglob("*.obj"), *root.rglob("*.o")])
    if not found:
        sys.exit("check_no_soft_double.py: no objects under %s - build the firmware first" % roots[0])
    return found


def calls_by_function(objdump, obj, *, root=REPO, addr2line=None, covered=None):
    """Called symbols per function after exempting annotated soft-double sites."""
    if covered is None:
        covered, _ = annotations(root)
    text = subprocess.run([objdump, "-dr", str(obj)], capture_output=True, text=True, check=True).stdout
    calls = {}
    soft_sites = {}
    for function, section, site, symbol in disassembly_calls(text, RELOCATION):
        if covered and SOFT_DOUBLE.match(symbol):
            soft_sites.setdefault(section, []).append((function, site, symbol))
        else:
            calls.setdefault(function, set()).add(symbol)
    for section, sites in soft_sites.items():
        frames = code_layout.source_frames(addr2line or xtensa_tool("addr2line"), obj,
                                          (site for _, site, _ in sites), section=section)
        for function, site, symbol in sites:
            if source_site(root, own_source(frames[site])) not in covered:
                calls.setdefault(function, set()).add(symbol)
    return calls


def offenders(calls):
    """The functions with remaining soft-double calls and no logging call."""
    found = {}
    for function, symbols in calls.items():
        soft = sorted(s for s in symbols if SOFT_DOUBLE.match(s))
        if soft and not any(LOGGING.match(s) for s in symbols):
            found[function] = soft
    return found


def main(argv):
    build = pathlib.Path(argv[1]) if len(argv) > 1 else REPO / "launcher" / "build.diag"
    objdump = xtensa_tool("objdump")
    covered, _ = annotations(REPO)
    bad = []
    count = 0
    for obj in objects(build):
        count += 1
        for function, soft in offenders(calls_by_function(objdump, obj, covered=covered)).items():
            bad.append("%s: %s calls %s" % (obj.name, function, ", ".join(soft)))
    if bad:
        print("soft-double routines called outside logging (the FPU is single precision):")
        print("\n".join("  " + b for b in bad))
        return 1
    print("no soft-double routine outside logging in %d objects" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
