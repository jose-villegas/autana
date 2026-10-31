#!/usr/bin/env python3
"""Fail when ESP-IDF's PSRAM timing tuning can run with the USB port on the bus.

    python launcher/tools/build/check_psram_tuning_wrap.py [build-dir]

At 120 MHz the tuning sweeps the PLL that clocks the USB PHY, so
main/board/board_usb.c wraps it (--wrap=mspi_timing_psram_tuning in
launcher/main/CMakeLists.txt) and takes the port off the bus for the call.
The wrap is a link-time redirect and fails silently: if ESP-IDF moves the
call next to the function, or the link option is lost, the image still links.
This reads the linked image instead. With CONFIG_SPIRAM_SPEED_120M in the
build's sdkconfig, the wrapper must be reached, the tuning must be called
directly by the wrapper alone, and the tuning's address must appear nowhere
in the loaded image, where an indirect call would load it from. Without
CONFIG_SPIRAM_SPEED_120M, the wrapper must not be linked.

The build directory defaults to launcher/build.diag; any build of the
firmware works. Needs the Xtensa objdump from ESP-IDF's toolchain.
"""

import json
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from espressif import xtensa_tool  # noqa: E402
from objdump_calls import function_addresses, parse_calls  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
TUNING = "mspi_timing_psram_tuning"
WRAPPER = "__wrap_" + TUNING
CALL = re.compile(r"\scall\d+\s+[0-9a-f]+ <([^>+]+)>")


def callers(text, callee):
    """The functions in `objdump -d` text that call `callee` directly."""
    return {function for function, called in parse_calls(text, CALL).items() if callee in called}


def holds_word(image, value):
    """True if `value` is one of the image's 32-bit words. A taken address
    sits word-aligned, and an image's segments keep file offsets aligned
    with load addresses, so bytes that match across two words are not one."""
    pattern = value.to_bytes(4, "little")
    at = image.find(pattern)
    while at != -1:
        if at % 4 == 0:
            return True
        at = image.find(pattern, at + 1)
    return False


def problems(text, sweeps, image=b""):
    """What is wrong with the image's tuning calls, as messages. `image` is
    the loaded image, where a taken function address sits as data."""
    addresses = function_addresses(text)
    taken = {name for name, address in addresses.items()
             if name in (TUNING, WRAPPER) and holds_word(image, address)}
    if not sweeps:
        return ["%s is linked although the PSRAM clock is not 120 MHz" % WRAPPER] if WRAPPER in addresses else []
    found = []
    if not callers(text, WRAPPER) and WRAPPER not in taken:
        found.append("nothing calls %s" % WRAPPER)
    stray = sorted(callers(text, TUNING) - {WRAPPER})
    if stray:
        found.append("%s is called outside the wrapper by %s" % (TUNING, ", ".join(stray)))
    if TUNING in taken:
        found.append("the address of %s is in the image, so an indirect call can bypass the wrapper" % TUNING)
    return found


def sweeps(build):
    config = build / "config" / "sdkconfig.json"
    if not config.is_file():
        sys.exit("check_psram_tuning_wrap.py: no %s - build the firmware first" % config)
    return bool(json.loads(config.read_text(encoding="utf-8")).get("SPIRAM_SPEED_120M"))


def main(argv):
    build = pathlib.Path(argv[1]) if len(argv) > 1 else REPO / "launcher" / "build.diag"
    elf, image = build / "launcher.elf", build / "launcher.bin"
    for path in (elf, image):
        if not path.is_file():
            sys.exit("check_psram_tuning_wrap.py: no %s - build the firmware first" % path)
    text = subprocess.run([xtensa_tool("objdump"), "-d", str(elf)], capture_output=True, text=True,
                          check=True).stdout
    at_120 = sweeps(build)
    found = problems(text, at_120, image.read_bytes())
    if found:
        print("PSRAM tuning can run with the USB port on the bus (main/board/board_usb.c):")
        print("\n".join("  " + f for f in found))
        return 1
    print("PSRAM tuning is reached only through %s" % WRAPPER if at_120 else "no PSRAM tuning sweep to wrap")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
