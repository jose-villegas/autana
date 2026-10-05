"""The panel's own size, read from the board header that owns it (bsp/display.h's
BSP_LCD_H_RES x BSP_LCD_V_RES, which gfx.h's GFX_WIDTH x GFX_HEIGHT are), so no
tool keeps a copy of its own that could drift."""

import re
from pathlib import Path

DISPLAY_HEADER = (Path(__file__).resolve().parents[2] / "components" / "esp32_s3_touch_amoled_1_8"
                  / "include" / "bsp" / "display.h")


def read_define(text, name):
    match = re.search(r"^#define\s+" + name + r"\s+\(?(\d+)\)?", text, re.MULTILINE)
    if match is None:
        raise RuntimeError(f"{DISPLAY_HEADER} defines no {name}")
    return int(match[1])


_header = DISPLAY_HEADER.read_text(encoding="utf-8")
PANEL_WIDTH = read_define(_header, "BSP_LCD_H_RES")
PANEL_HEIGHT = read_define(_header, "BSP_LCD_V_RES")
