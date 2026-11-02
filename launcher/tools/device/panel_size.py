"""The panel's own size, read from the board header that owns it (bsp/display.h's
BSP_LCD_H_RES x BSP_LCD_V_RES, which gfx.h's GFX_WIDTH x GFX_HEIGHT are), so no
tool keeps a copy of its own that could drift."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build.c_header import read_define

DISPLAY_HEADER = (Path(__file__).resolve().parents[2] / "components" / "esp32_s3_touch_amoled_1_8"
                  / "include" / "bsp" / "display.h")

PANEL_WIDTH = read_define(DISPLAY_HEADER, "BSP_LCD_H_RES")
PANEL_HEIGHT = read_define(DISPLAY_HEADER, "BSP_LCD_V_RES")
