#!/usr/bin/env python3
"""Bake an authored screen layout into a fixed firmware table.

    python tools/gen/gen_ui_layout.py main/ui/<screen>_layout.json \\
        main/ui/<screen>_layout_generated.h [--check]

The JSON names its own screen and elements, so one generator serves every
authored screen. Every C identifier in the output derives from `screen`.
"""

import argparse
import json
import re
from pathlib import Path


SCHEMA_VERSION = 2
ORIENTATIONS = ("portrait", "landscape")
CANVASES = {"portrait": (368, 448), "landscape": (448, 368)}
MIN_TAP_TARGET = 56  # ui/ui.h UI_TAP_MIN
IDENTIFIER = re.compile(r"^[a-z][a-z0-9_]*$")


def fail(message):
    raise ValueError(f"gen_ui_layout.py: {message}")


def overlaps(first, second):
    ax, ay, aw, ah = first
    bx, by, bw, bh = second
    return ax < bx + bw and bx < ax + aw and ay < by + bh and by < ay + ah


def validate_elements(document):
    elements = document.get("elements")
    if not isinstance(elements, list) or not elements:
        fail("elements must be a non-empty list")
    ids = []
    for element in elements:
        if not isinstance(element, dict) or set(element) != {"id", "label", "interactive"}:
            fail("each element needs exactly id, label and interactive")
        element_id = element["id"]
        if not isinstance(element_id, str) or not IDENTIFIER.match(element_id):
            fail(f"element id {element_id!r} is not a lower_snake_case identifier")
        if element_id in ids:
            fail(f"element id {element_id} is repeated")
        if not isinstance(element["label"], str) or not element["label"]:
            fail(f"{element_id}.label must be a non-empty string")
        if type(element["interactive"]) is not bool:
            fail(f"{element_id}.interactive must be true or false")
        ids.append(element_id)
    return elements


def validate_orientation(name, authored, elements):
    canvas = CANVASES[name]
    if not isinstance(authored, dict) or authored.get("canvas") != list(canvas):
        fail(f"{name}.canvas must be {list(canvas)}")
    ids = [element["id"] for element in elements]
    source_rects = authored.get("rects")
    if not isinstance(source_rects, dict) or set(source_rects) != set(ids):
        fail(f"{name}.rects must contain exactly the declared element ids")

    rects = {}
    for element in elements:
        element_id = element["id"]
        rect = source_rects[element_id]
        if not isinstance(rect, list) or len(rect) != 4 or any(type(value) is not int for value in rect):
            fail(f"{name}.{element_id} must be four integers")
        x, y, width, height = rect
        if x < 0 or y < 0 or width <= 0 or height <= 0 or x + width > canvas[0] or y + height > canvas[1]:
            fail(f"{name}.{element_id} leaves the canvas")
        if element["interactive"] and (width < MIN_TAP_TARGET or height < MIN_TAP_TARGET):
            fail(f"{name}.{element_id} is smaller than the {MIN_TAP_TARGET}px tap target")
        rects[element_id] = tuple(rect)
    for index, first_id in enumerate(ids):
        for second_id in ids[index + 1 :]:
            if overlaps(rects[first_id], rects[second_id]):
                fail(f"{name}.{first_id} overlaps {second_id}")
    return canvas, rects


def validate(document):
    if not isinstance(document, dict):
        fail("document root must be an object")
    if document.get("schema_version") != SCHEMA_VERSION:
        fail(f"schema_version must be {SCHEMA_VERSION}")
    screen = document.get("screen")
    if not isinstance(screen, str) or not IDENTIFIER.match(screen):
        fail("screen must be a lower_snake_case identifier")
    if not isinstance(document.get("title"), str) or not document["title"]:
        fail("title must be a non-empty string")
    elements = validate_elements(document)
    orientations = document.get("orientations")
    if not isinstance(orientations, dict) or set(orientations) != set(ORIENTATIONS):
        fail("orientations must contain exactly portrait and landscape")
    return {name: validate_orientation(name, orientations[name], elements) for name in ORIENTATIONS}


def generate(document):
    layouts = validate(document)
    screen = document["screen"]
    prefix = screen.upper() + "_ELEMENT_"
    ids = [element["id"] for element in document["elements"]]
    lines = [
        "/*",
        " * GENERATED FILE - do not edit.",
        " *",
        f" *     python tools/gen/gen_ui_layout.py main/ui/{screen}_layout.json main/ui/{screen}_layout_generated.h",
        " */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "typedef enum {",
    ]
    lines.extend(f"    {prefix}{element_id.upper()} = {index}," for index, element_id in enumerate(ids))
    lines.extend(
        [
            f"    {prefix}COUNT = {len(ids)}",
            f"}} {screen}_element_id_t;",
            "",
            "typedef struct {",
            "    int16_t x, y, width, height;",
            f"}} {screen}_layout_rect_t;",
            "",
            "typedef struct {",
            "    int16_t canvas_width, canvas_height;",
            f"    {screen}_layout_rect_t rects[{prefix}COUNT];",
            f"}} {screen}_layout_t;",
            "",
        ]
    )
    for orientation in ORIENTATIONS:
        canvas, rects = layouts[orientation]
        lines.append(f"static const {screen}_layout_t {screen}_layout_{orientation} = {{")
        lines.append(f"    .canvas_width = {canvas[0]},")
        lines.append(f"    .canvas_height = {canvas[1]},")
        lines.append("    .rects =")
        lines.append("        {")
        for element_id in ids:
            values = ", ".join(map(str, rects[element_id]))
            lines.append(f"            [{prefix}{element_id.upper()}] = {{{values}}},")
        lines.extend(["        },", "};", ""])
    return "\n".join(lines).rstrip() + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true", help="fail if the output file is stale")
    args = parser.parse_args()
    try:
        baked = generate(json.loads(args.source.read_text(encoding="utf-8")))
        if args.check:
            if args.output.read_text(encoding="utf-8") != baked:
                parser.exit(1, f"{args.output} is stale; regenerate it\n")
        else:
            args.output.write_text(baked, encoding="utf-8", newline="\n")
    except (OSError, json.JSONDecodeError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
