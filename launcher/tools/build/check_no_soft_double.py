#!/usr/bin/env python3
"""Fail when firmware code calls a soft-double routine outside logging.

    python launcher/tools/build/check_no_soft_double.py [build-dir]

The ESP32-S3's FPU is single precision only, so every `double` operation is a
libgcc call (__adddf3, __muldf3, __divdf3, __extendsfdf2, __truncdfsf2, ...)
at about ten times the cost of the float one. -Werror=double-promotion and
-Werror=float-conversion (launcher/main/CMakeLists.txt) stop the implicit
ones at compile time; this reads the objects of the `main` component and
finds the explicit ones the compiler accepts, such as a `(double)` cast or a
double constant, by the function that calls them.

A function may call a soft-double routine only when it also calls a logging
or formatting routine (esp_log, printf, snprintf, ...): a `%f` argument
is a double by the language, and the call itself is the evidence. Every other
function with one fails by name, in any file, hot path or not. There is no
list of exempt files or functions: the exemption is the function's own call.

The build directory defaults to launcher/build.diag; any build of the
firmware works. Needs the Xtensa objdump from ESP-IDF's toolchain.
"""

import pathlib
import re
import shutil
import subprocess
import sys

REPO = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

SOFT_DOUBLE = re.compile(r"^__\w*df\w*$")
LOGGING = re.compile(r"^(esp_log|esp_log_write|esp_log_writev|printf|snprintf|vsnprintf|sprintf|esp_rom_printf|"
                     r"ets_printf|fprintf|vprintf|__wrap_\w*printf)$")
FUNCTION = re.compile(r"^[0-9a-f]+ <([^>]+)>:\s*$")
RELOCATION = re.compile(r"^\s*[0-9a-f]+:\s+R_XTENSA_\w+\s+(\S+)")


def find_objdump():
    found = shutil.which("xtensa-esp32s3-elf-objdump")
    if found:
        return found
    from espressif import espressif_tools_root
    pattern = "tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-objdump*"
    for candidate in sorted(espressif_tools_root().glob(pattern)):
        if candidate.is_file():
            return str(candidate)
    sys.exit("check_no_soft_double.py: no xtensa-esp32s3-elf-objdump on PATH or under the ESP-IDF tools")


def objects(build):
    root = build / "esp-idf" / "main" / "CMakeFiles" / "__idf_main.dir"
    found = sorted(list(root.rglob("*.obj")) + list(root.rglob("*.o")))
    if not found:
        sys.exit("check_no_soft_double.py: no objects under %s - build the firmware first" % root)
    return found


def calls_by_function(objdump, obj):
    """{function: set of called symbols} for one object file."""
    text = subprocess.run([objdump, "-dr", str(obj)], capture_output=True, text=True, check=True).stdout
    calls = {}
    current = None
    for line in text.splitlines():
        m = FUNCTION.match(line)
        if m:
            # The literal pool a function loads its calls from is a section of
            # its own, named for the function: they are one function here.
            current = calls.setdefault(m.group(1).removeprefix(".literal."), set())
            continue
        m = RELOCATION.match(line)
        if m and current is not None:
            current.add(re.sub(r"[+-]0x[0-9a-f]+$", "", m.group(1)))
    return calls


def offenders(calls):
    """The functions calling soft double without a logging call beside it."""
    found = {}
    for function, symbols in calls.items():
        soft = sorted(s for s in symbols if SOFT_DOUBLE.match(s))
        if soft and not any(LOGGING.match(s) for s in symbols):
            found[function] = soft
    return found


def main(argv):
    build = pathlib.Path(argv[1]) if len(argv) > 1 else REPO / "launcher" / "build.diag"
    objdump = find_objdump()
    bad = []
    count = 0
    for obj in objects(build):
        count += 1
        for function, soft in offenders(calls_by_function(objdump, obj)).items():
            bad.append("%s: %s calls %s" % (obj.name, function, ", ".join(soft)))
    if bad:
        print("soft-double routines called outside logging (the FPU is single precision):")
        print("\n".join("  " + b for b in bad))
        return 1
    print("no soft-double routine outside logging in %d objects" % count)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
