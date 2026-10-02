#!/usr/bin/env python3
"""Fail when launcher/main/main.c includes or calls firmware directly.

    python scripts/gates/check_shell_firmware.py

main.c is the shell: it starts the platform and runs the frame loop, and
everything hardware-facing sits behind a module of its own (input/, display/,
util/timing.h, util/settings.h, util/memory.h, util/log.h), each with a
device half in a *_device.c file. A firmware call left in main.c is a second
place that knows the chip, so this fails on

  - an include of an ESP-IDF, FreeRTOS, NVS, BSP, driver, HAL or SoC header,
    or of a driver header under input/, and
  - any use of a name those pull in: the ESP-IDF, NVS, heap, BSP and FreeRTOS
    prefixes below, and every function, type and macro a driver header
    declares.

A driver header is the header beside an input/*.c that itself includes a
firmware header (touch.h, buttons.h, imu.h today), found by looking, so a
new driver is covered the day it is added.

Comments and string literals are not code. There is no exemption list: a
reason for main.c to touch the firmware is a missing module, so add one.
"""
import pathlib
import re
import sys

MAIN = "launcher/main/main.c"
INPUT_DIR = "launcher/main/input"

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
FIRMWARE_INCLUDE_RE = re.compile(r"^(esp_|nvs|freertos/|bsp/|driver/|hal/|soc/|rom/)")
FIRMWARE_NAME_RE = re.compile(
    r"^(esp_|ESP_|nvs_|NVS_|heap_caps_|MALLOC_CAP_|bsp_|BSP_|vTask|xTask|xQueue|xSemaphore|xEvent|"
    r"portMAX_DELAY$|pdMS_TO_TICKS$|pdTRUE$|pdFALSE$|pdPASS$)")

IDENTIFIER_RE = re.compile(r"[A-Za-z_]\w*")
C_WORDS = frozenset("""if else for while do switch case default return sizeof typedef struct enum union
    static inline const extern void bool int char unsigned signed long short float double defined
    _Static_assert _Bool""".split())

COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.DOTALL)
COMMENT_OR_STRING_RE = re.compile(
    COMMENT_RE.pattern + r"""|"(?:\\.|[^"\\\n])*"|'(?:\\.|[^'\\\n])*'""", re.DOTALL)
DEFINE_RE = re.compile(r"^\s*#\s*define\s+([A-Za-z_]\w*)", re.MULTILINE)
FUNCTION_RE = re.compile(
    r"\b([A-Za-z_]\w*)\s*\([^;{}()]*(?:\([^()]*\)[^;{}()]*)*\)\s*(?:__attribute__\s*\(\(.*?\)\)\s*)*[;{]")
TYPEDEF_NAME_RE = re.compile(r"\}\s*([A-Za-z_]\w*)\s*;|\btypedef\b[^;{}]*?\b([A-Za-z_]\w*)\s*;")
ENUM_CONSTANT_RE = re.compile(r"^\s*([A-Z][A-Z0-9_]*)\s*(?:=[^,\n]*)?(?:,|$)", re.MULTILINE)


def blank(match):
    """The text with the same line breaks, so a line number survives."""
    return "".join(c if c == "\n" else " " for c in match.group(0))


def strip_comments_and_strings(text):
    return COMMENT_OR_STRING_RE.sub(blank, text)


def driver_headers(root):
    """The headers (repo-relative) beside the input/*.c files that include a
    firmware header."""
    found = []
    for source in sorted((root / INPUT_DIR).glob("*.c")):
        text = COMMENT_RE.sub(blank, source.read_text(encoding="utf-8", errors="replace"))
        includes = (INCLUDE_RE.match(line) for line in text.splitlines())
        if any(m and FIRMWARE_INCLUDE_RE.match(m.group(1)) for m in includes):
            header = source.with_suffix(".h")
            if header.is_file():
                found.append(f"{INPUT_DIR}/{header.name}")
    return found


def driver_names(root, headers):
    """Every function, type, macro and enum constant the driver headers
    declare."""
    names = set()
    for header in headers:
        text = strip_comments_and_strings((root / header).read_text(encoding="utf-8", errors="replace"))
        names.update(DEFINE_RE.findall(text))
        names.update(name for name in (m.group(1) for m in FUNCTION_RE.finditer(text)) if name not in C_WORDS)
        for match in TYPEDEF_NAME_RE.finditer(text):
            names.add(match.group(1) or match.group(2))
        names.update(ENUM_CONSTANT_RE.findall(text))
    return names - C_WORDS


def problems(root="."):
    """The `path:line: reason` lines for main.c, in line order."""
    root = pathlib.Path(root)
    main = root / MAIN
    if not main.is_file():
        return [f"{MAIN}: not found, so nothing was checked"]
    headers = driver_headers(root)
    if not headers:
        return [f"{INPUT_DIR}: no driver header found, so no driver name can be checked"]
    names = driver_names(root, headers)
    found = []
    text = main.read_text(encoding="utf-8", errors="replace")
    without_comments = COMMENT_RE.sub(blank, text).splitlines()
    code = strip_comments_and_strings(text).splitlines()
    for number, line in enumerate(code, 1):
        include = INCLUDE_RE.match(without_comments[number - 1])
        if include:
            header = include.group(1)
            if FIRMWARE_INCLUDE_RE.match(header) or f"launcher/main/{header}" in headers:
                found.append(f"{MAIN}:{number}: includes {header}")
            continue
        for identifier in dict.fromkeys(IDENTIFIER_RE.findall(line)):
            if FIRMWARE_NAME_RE.match(identifier) or identifier in names:
                found.append(f"{MAIN}:{number}: uses {identifier}")
    return found


def main(argv):
    if argv:
        print("usage: check_shell_firmware.py", file=sys.stderr)
        return 2
    found = problems(".")
    for line in found:
        print(line)
    print(f"{len(found)} firmware use(s) in {MAIN}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
