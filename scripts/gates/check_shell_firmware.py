#!/usr/bin/env python3
"""Fail when the shell includes or calls vendor firmware directly, or when
anything but its owner or a driver calls the vendor clock or heap.

    python scripts/gates/check_shell_firmware.py

The rule holds launcher/main/main.c, which starts the platform, and every
file under launcher/main/shell/, which runs the frame loop, to one standard.
The chip's vendor code (ESP-IDF, FreeRTOS, NVS, the board support package)
sits behind a module of this firmware's own (input/, display/,
util/timing.h, util/settings.h, util/memory.h). A vendor call left in the
shell is a second place that knows the chip, so this fails on

  - an include of an esp_*, nvs*, freertos/, bsp/, driver/, hal/, soc/ or
    rom/ header, and
  - any use of a name those pull in: the ESP-IDF, NVS, heap, BSP and FreeRTOS
    prefixes below.

This firmware's own headers, drivers included, are not vendor code. Logging
is not firmware: esp_log.h and the ESP_LOG[A-Z] macros are carved out by the
patterns themselves, not by a list. Comments and string literals are not
code. There is no exemption list: a reason for the shell to touch the vendor
code is a missing module, so add one.

The vendor timer and heap have owners: esp_timer_* belongs to util/timing,
and heap_caps_* and MALLOC_CAP_* to util/memory. In the firmware, its suites
and its tools, a name of either is code only in its owner's own files
(util/timing.* and util/timing_*.*, the same for memory) or in a driver:
anything under board/, or a *_device.c in launcher/main/ outside a tests/
folder, where the name means a suite that runs on the board. launcher/test/
outside suites/ (the host heap model, the stubs, the harness the board also
builds) stands in for the vendor code or measures from beneath it, so it is
not checked; nor is the shell, which the rule above holds to more.
"""
import pathlib
import re
import sys

from tracked import tracked_files

MAIN = "launcher/main/main.c"
SHELL = "launcher/main/shell/"

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


def is_shell(rel):
    return rel == MAIN or (rel.startswith(SHELL) and rel.endswith((".c", ".h")))


def file_problems(root, rel):
    """The `path:line: reason` lines for one file of the shell, in line order."""
    text = (pathlib.Path(root) / rel).read_text(encoding="utf-8", errors="replace")
    without_comments = COMMENT_RE.sub(blank, text).splitlines()
    code = COMMENT_OR_STRING_RE.sub(blank, text).splitlines()
    found = []
    for number, line in enumerate(code, 1):
        include = INCLUDE_RE.match(without_comments[number - 1])
        if include:
            if VENDOR_INCLUDE_RE.match(include.group(1)):
                found.append(f"{rel}:{number}: includes {include.group(1)}")
            continue
        for identifier in dict.fromkeys(IDENTIFIER_RE.findall(line)):
            if VENDOR_NAME_RE.match(identifier):
                found.append(f"{rel}:{number}: uses {identifier}")
    return found


def problems(root="."):
    """The `path:line: reason` lines for main.c and the files under shell/,
    main.c first, then in path then line order."""
    if not (pathlib.Path(root) / MAIN).is_file():
        return [f"{MAIN}: not found, so nothing was checked"]
    found = file_problems(root, MAIN)
    for rel in sorted(tracked_files(root)):
        if rel != MAIN and is_shell(rel):
            found.extend(file_problems(root, rel))
    return found


OWNED_NAME_RE = re.compile(r"\b(esp_timer_|heap_caps_|MALLOC_CAP_)\w*")
OWNER_OF = {"esp_timer_": "timing", "heap_caps_": "memory", "MALLOC_CAP_": "memory"}
CHECKED = ("launcher/main/", "launcher/test/suites/", "launcher/tools/")
UTIL = "launcher/main/util/"


def may_use(rel, prefix):
    """True when `rel` owns names with `prefix`, or is a driver. A suite's
    *_device.c is a test that runs on the board, not a driver."""
    if rel.startswith("launcher/main/board/"):
        return True
    if rel.startswith("launcher/main/") and rel.endswith("_device.c") and "/tests/" not in rel:
        return True
    if not rel.startswith(UTIL) or "/" in rel[len(UTIL):]:
        return False
    module = OWNER_OF[prefix]
    stem = pathlib.PurePosixPath(rel).stem
    return stem == module or stem.startswith(module + "_")


def owner_problems(root="."):
    """The `path:line: reason` lines for clock and heap names outside their
    owners and the drivers, in path then line order."""
    found = []
    for rel in tracked_files(root):
        if not rel.startswith(CHECKED) or not rel.endswith((".c", ".h")) or is_shell(rel):
            continue
        text = (pathlib.Path(root) / rel).read_text(encoding="utf-8", errors="replace")
        code = COMMENT_OR_STRING_RE.sub(blank, text).splitlines()
        for number, line in enumerate(code, 1):
            for name in dict.fromkeys(m.group(0) for m in OWNED_NAME_RE.finditer(line)):
                prefix = OWNED_NAME_RE.match(name).group(1)
                if not may_use(rel, prefix):
                    found.append(f"{rel}:{number}: uses {name}; only util/{OWNER_OF[prefix]} and a driver may")
    return found


def main(argv):
    if argv:
        print("usage: check_shell_firmware.py", file=sys.stderr)
        return 2
    found = problems(".")
    for line in found:
        print(line)
    print(f"{len(found)} vendor firmware use(s) in {MAIN} and {SHELL}")
    owned = owner_problems(".")
    for line in owned:
        print(line)
    print(f"{len(owned)} clock or heap use(s) outside util/timing, util/memory and the drivers")
    return 1 if found or owned else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
