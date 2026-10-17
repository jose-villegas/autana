#!/usr/bin/env python3
"""Fail when ESP-IDF's PSRAM timing tuning can run with the USB port on the bus.

    python launcher/tools/build/check_psram_tuning_wrap.py [build-dir]

At 120 MHz the tuning sweeps the PLL that clocks the USB PHY, so
main/board/board_usb.c wraps it (--wrap=mspi_timing_psram_tuning in
launcher/main/CMakeLists.txt) and takes the port off the bus for the call.
The wrap is a link-time redirect and fails silently: if ESP-IDF moves the
call next to the function, or the link option is lost, the image still links.
This reads the linked image's calls instead. With CONFIG_SPIRAM_SPEED_120M in
the build's sdkconfig, the wrapper must be called and the tuning must be
called from the wrapper alone; without it, the wrapper must not be linked.

The build directory defaults to launcher/build.diag; any build of the
firmware works. Needs the Xtensa objdump from ESP-IDF's toolchain.
"""

import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from check_no_soft_double import REPO, find_objdump  # noqa: E402

TUNING = "mspi_timing_psram_tuning"
WRAPPER = "__wrap_" + TUNING
FUNCTION = re.compile(r"^[0-9a-f]+ <([^>]+)>:\s*$")
CALL = re.compile(r"\scall[x]?\d+\s+[0-9a-f]+ <([^>+]+)>")


def callers(text, callee):
    """The functions in `objdump -d` text that call `callee` directly."""
    found = set()
    current = None
    for line in text.splitlines():
        m = FUNCTION.match(line)
        if m:
            current = m.group(1)
            continue
        m = CALL.search(line)
        if m and m.group(1) == callee and current is not None:
            found.add(current)
    return found


def problems(text, sweeps):
    """What is wrong with the image's tuning calls, as messages."""
    to_wrapper = callers(text, WRAPPER)
    to_tuning = callers(text, TUNING)
    if not sweeps:
        return ["%s is linked although the PSRAM clock is not 120 MHz" % WRAPPER] if to_wrapper else []
    found = []
    if not to_wrapper:
        found.append("nothing calls %s" % WRAPPER)
    stray = sorted(to_tuning - {WRAPPER})
    if stray:
        found.append("%s is called outside the wrapper by %s" % (TUNING, ", ".join(stray)))
    return found


def sweeps(build):
    config = build / "config" / "sdkconfig.json"
    if not config.is_file():
        sys.exit("check_psram_tuning_wrap.py: no %s - build the firmware first" % config)
    return bool(json.loads(config.read_text(encoding="utf-8")).get("SPIRAM_SPEED_120M"))


def main(argv):
    build = pathlib.Path(argv[1]) if len(argv) > 1 else REPO / "launcher" / "build.diag"
    elf = build / "launcher.elf"
    if not elf.is_file():
        sys.exit("check_psram_tuning_wrap.py: no %s - build the firmware first" % elf)
    text = subprocess.run([find_objdump(), "-d", str(elf)], capture_output=True, text=True, check=True).stdout
    found = problems(text, sweeps(build))
    if found:
        print("PSRAM tuning can run with the USB port on the bus (main/board/board_usb.c):")
        print("\n".join("  " + f for f in found))
        return 1
    print("PSRAM tuning is reached only through %s" % WRAPPER if sweeps(build) else "no PSRAM tuning sweep to wrap")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
