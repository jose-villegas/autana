#!/usr/bin/env python3
"""Fail when launcher/main/main.c includes or calls vendor firmware directly.

    python scripts/gates/check_shell_firmware.py

main.c is the shell: it starts the platform and runs the frame loop, and the
chip's vendor code (ESP-IDF, FreeRTOS, NVS, the board support package) sits
behind a module of this firmware's own (input/, display/, util/timing.h,
util/settings.h, util/memory.h), each with a device half in a *_device.c
file. A vendor call left in main.c is a second place that knows the chip, so
this fails on

  - an include of an esp_*, nvs*, freertos/, bsp/, driver/, hal/, soc/ or
    rom/ header, and
  - any use of a name those pull in: the ESP-IDF, NVS, heap, BSP and FreeRTOS
    prefixes below.

This firmware's own headers, drivers included, are not vendor code. Logging
is not firmware: esp_log.h and the ESP_LOG[A-Z] macros are carved out by the
patterns themselves, not by a list. Comments and string literals are not
code. There is no exemption list: a reason for main.c to touch the vendor
code is a missing module, so add one.
"""
import pathlib
import re
import sys

MAIN = "launcher/main/main.c"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
VENDOR_INCLUDE_RE = re.compile(r"^(esp_(?!log\.h$)|nvs|freertos/|bsp/|driver/|hal/|soc/|rom/)")
VENDOR_NAME_RE = re.compile(
    r"^(esp_|ESP_(?!LOG[A-Z]$)|nvs_|NVS_|heap_caps_|MALLOC_CAP_|bsp_|BSP_"
    r"|(v|x|ux|ul|pv|pc)[A-Z]|(port|config)[A-Z]|pd[A-Z]"
    r"|(Task|Queue|Semaphore|Event|Timer|Stream|Message)\w*Handle_t$|(U?Base|Tick|Stack)Type_t$)")

IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*")
COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
COMMENT_OR_STRING_RE = re.compile(
    COMMENT_RE.pattern + r"""|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.DOTALL)


def blank(match):
    """The text with the same line breaks, so a line number survives."""
    return "".join(c if c == "\n" else " " for c in match.group(0))


def problems(root="."):
    """The `path:line: reason` lines for main.c, in line order."""
    main = pathlib.Path(root) / MAIN
    if not main.is_file():
        return [f"{MAIN}: not found, so nothing was checked"]
    text = main.read_text(encoding="utf-8", errors="replace")
    without_comments = COMMENT_RE.sub(blank, text).splitlines()
    code = COMMENT_OR_STRING_RE.sub(blank, text).splitlines()
    found = []
    for number, line in enumerate(code, 1):
        include = INCLUDE_RE.match(without_comments[number - 1])
        if include:
            if VENDOR_INCLUDE_RE.match(include.group(1)):
                found.append(f"{MAIN}:{number}: includes {include.group(1)}")
            continue
        for identifier in dict.fromkeys(IDENTIFIER_RE.findall(line)):
            if VENDOR_NAME_RE.match(identifier):
                found.append(f"{MAIN}:{number}: uses {identifier}")
    return found


def main(argv):
    if argv:
        print("usage: check_shell_firmware.py", file=sys.stderr)
        return 2
    found = problems(".")
    for line in found:
        print(line)
    print(f"{len(found)} vendor firmware use(s) in {MAIN}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
