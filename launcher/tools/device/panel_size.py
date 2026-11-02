"""The panel's own size, read from the board header that owns it (the chosen board's
board_config.h, BOARD_PANEL_WIDTH x BOARD_PANEL_HEIGHT, which gfx.h's GFX_WIDTH x GFX_HEIGHT
are), so no tool keeps a copy of its own that could drift."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build.c_header import read_define
from build.packages import board_include_dir

CONFIG_HEADER = board_include_dir() / "board_config.h"

PANEL_WIDTH = read_define(CONFIG_HEADER, "BOARD_PANEL_WIDTH")
PANEL_HEIGHT = read_define(CONFIG_HEADER, "BOARD_PANEL_HEIGHT")
