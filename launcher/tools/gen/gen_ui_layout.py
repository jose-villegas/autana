#!/usr/bin/env python3
"""Bake an authored screen layout into a fixed firmware table.

    python tools/gen/gen_ui_layout.py main/ui/<screen>_layout.json \\
        main/ui/<screen>_layout_generated.h [--check]

The JSON names its own screen and elements, so one generator serves every
authored screen. Every C identifier in the output derives from `screen`.
Validation requires the built editor_layout_validate executable; --validator
or AUTANA_LAYOUT_VALIDATOR selects a non-default build directory.
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))
from panel_size import PANEL_HEIGHT, PANEL_WIDTH, read_define  # noqa: E402  (path must be set up first)


ORIENTATIONS = ("portrait", "landscape")
CANVASES = {"portrait": (PANEL_WIDTH, PANEL_HEIGHT), "landscape": (PANEL_HEIGHT, PANEL_WIDTH)}
MIN_TAP_TARGET = read_define((Path(__file__).resolve().parents[2] / "main" / "ui" / "ui.h").read_text(encoding="utf-8"), "UI_TAP_MIN")


def validate(document, validator=None):
    executable = validator or os.environ.get("AUTANA_LAYOUT_VALIDATOR") or (
        Path(__file__).resolve().parents[3] / "editor" / "build" /
        ("editor_layout_validate.exe" if os.name == "nt" else "editor_layout_validate"))
    with tempfile.TemporaryDirectory(prefix="ui-layout-") as directory:
        source = Path(directory) / "layout.json"
        source.write_text(json.dumps(document), encoding="utf-8")
        try:
            result = subprocess.run([str(executable), str(source)], capture_output=True, text=True, check=False)
        except OSError as error:
            raise ValueError(f"Build editor_layout_validate or set AUTANA_LAYOUT_VALIDATOR: {error}") from error
        if result.returncode:
            raise ValueError(result.stderr.strip())
    return {name: (CANVASES[name], document["orientations"][name]["rects"]) for name in ORIENTATIONS}


def generate(document, validator=None):
    layouts = validate(document, validator)
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
    parser.add_argument("--validator", type=Path, help="built editor_layout_validate executable")
    args = parser.parse_args()
    try:
        baked = generate(json.loads(args.source.read_text(encoding="utf-8")), args.validator)
        if args.check:
            if args.output.read_text(encoding="utf-8") != baked:
                parser.exit(1, f"{args.output} is stale; regenerate it\n")
        else:
            args.output.write_text(baked, encoding="utf-8", newline="\n")
    except (OSError, json.JSONDecodeError, ValueError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    main()
