"""What the kinematics figure scripts share: the tools and math folders on the path, and the panel,
view and helpers of math_figures.py, so each script imports one module before its own."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "math"))
from math_figures import (DISPLAY, PANEL_SIZE, PANEL_TITLE_ROOM, ROOT,  # noqa: E402,F401
                          VIEW_AZIMUTH, VIEW_ELEVATION, read_define)
