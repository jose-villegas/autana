#!/usr/bin/env python3
"""Fail when the shell includes or calls vendor firmware directly, or when
anything but its owner or a driver calls the vendor clock or heap.

    python scripts/gates/check_shell_firmware.py

The rule holds launcher/main/main.c, which starts the platform, and every
file under launcher/main/shell/, which runs the frame loop, to one standard.
The chip's vendor code (ESP-IDF, FreeRTOS, NVS, the board support package)
sits behind a module of this firmware's own (input/, display/,
core/timing.h, services/settings.h, core/memory.h). A
vendor call left in the shell is a second place that knows the chip, so this fails on

  - an include of an esp_*, nvs*, freertos/, bsp/, driver/, hal/, soc/ or
    rom/ header, and
  - any use of a name those pull in: the ESP-IDF, NVS, heap, BSP and FreeRTOS
    prefixes below.

This firmware's own headers, drivers included, are not vendor code. Logging
is not firmware: esp_log.h and the ESP_LOG[A-Z] macros are carved out by the
patterns themselves, not by a list. Comments and string literals are not
code. There is no exemption list: a reason for the shell to touch the vendor
code is a missing module, so add one.

The vendor clock, pause and heap have owners: esp_timer_* and vTaskDelay
belong to core/timing, and heap_caps_*, MALLOC_CAP_*, multi_heap_*,
esp_psram_* and the esp_get_*heap_size queries to core/memory.
In launcher/main/, launcher/test/suites/ and launcher/tools/, these names
are code only in their owner's files (core/timing.* and core/timing_*.*, the
same for memory) or in a driver: anything under board/, or a *_device.c in
launcher/main/ outside a tests/ folder, where the name means a suite that runs
on the board. The board support package is one board's, so it belongs to
board/ alone, drivers included: a bsp/ include or a bsp_, BSP_ or CONFIG_BSP_
name anywhere else fails. launcher/test/ outside suites/ (the host heap model,
the stubs, the harness the board also builds) stands in for the vendor code
or measures from beneath it, so it is not checked; nor is the shell, which
the rule above holds to more. Standalone packages under launcher/packages/
have their own platform shims and do not use the engine's clock and heap
owners.
"""
import pathlib
import re
import sys

from tracked import tracked_files

MAIN = "launcher/main/main.c"
SHELL = "launcher/main/shell/"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
VENDOR_INCLUDE_RE = re.compile(r"^(esp_(?!log\.h$)|nvs|freertos/|bsp/|driver/|hal/|soc/|rom/|multi_heap)")
VENDOR_NAME_RE = re.compile(
    r"^(esp_|ESP_(?!LOG[A-Z]$)|nvs_|NVS_|heap_caps_|MALLOC_CAP_|multi_heap_|bsp_|BSP_"
    r"|(v|x|ux|ul|pv|pc)[A-Z]|(port|config)[A-Z]|pd[A-Z]"
    r"|(Task|Queue|Semaphore|Event|Timer|Stream|Message)\w*Handle_t$|(U?Base|Tick|Stack)Type_t$)")

# The boundary keeps the x of a hex literal such as 0xFU from reading as a name.
IDENTIFIER_RE = re.compile(r"\b[A-Za-z_]\w*")
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


OWNED_NAME_RE = re.compile(r"\b(?:(?P<timing>esp_timer_\w*|vTaskDelay\b)"
                           r"|(?P<memory>(?:heap_caps_|MALLOC_CAP_|multi_heap_|esp_psram_)\w*|esp_get_\w*heap_size\b)"
                           r"|(?P<board>(?:bsp_|BSP_|CONFIG_BSP_)\w*))")
BSP_INCLUDE_RE = re.compile(r"^bsp/")
CHECKED = ("launcher/main/", "launcher/test/suites/", "launcher/tools/")
CORE = "launcher/main/core/"
BOARD = "launcher/main/board/"


def may_use(rel, module):
    """True when `rel` is one of core/`module`'s files, or is a driver. A
    suite's *_device.c is a test that runs on the board, not a driver. The
    BSP is board/'s alone."""
    if rel.startswith(BOARD):
        return True
    if module == "board":
        return False
    if rel.startswith("launcher/main/") and rel.endswith("_device.c") and "/tests/" not in rel:
        return True
    if not rel.startswith(CORE) or "/" in rel[len(CORE):]:
        return False
    stem = pathlib.PurePosixPath(rel).stem
    return stem == module or stem.startswith(module + "_")


def owner_reason(module):
    return "only board/ may" if module == "board" else f"only core/{module} and a driver may"


def owner_problems(root="."):
    """The `path:line: reason` lines for clock, heap and BSP names outside
    their owners, in path then line order."""
    found = []
    for rel in tracked_files(root):
        if not rel.startswith(CHECKED) or not rel.endswith((".c", ".h")) or is_shell(rel):
            continue
        text = (pathlib.Path(root) / rel).read_text(encoding="utf-8", errors="replace")
        without_comments = COMMENT_RE.sub(blank, text).splitlines()
        code = COMMENT_OR_STRING_RE.sub(blank, text).splitlines()
        for number, line in enumerate(code, 1):
            include = INCLUDE_RE.match(without_comments[number - 1])
            if include:
                if BSP_INCLUDE_RE.match(include.group(1)) and not may_use(rel, "board"):
                    found.append(f"{rel}:{number}: includes {include.group(1)}; {owner_reason('board')}")
                continue
            for name, module in dict.fromkeys((m.group(0), m.lastgroup) for m in OWNED_NAME_RE.finditer(line)):
                if not may_use(rel, module):
                    found.append(f"{rel}:{number}: uses {name}; {owner_reason(module)}")
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
    print(f"{len(owned)} clock, heap or BSP use(s) outside their owners")
    return 1 if found or owned else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
