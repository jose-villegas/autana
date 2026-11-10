"""LCRV record layouts, read without numpy so the pack builder can mirror them; render/curve/r3d_curve.h owns them."""

import pathlib
import struct

from build.c_header import read_define
from r3d.mesh_asset import checked_struct

OWNER = pathlib.Path(__file__).resolve().parents[3] / "main/render/curve/r3d_curve.h"
TYPE = b"LCRV"
NAME_SUFFIX = read_define(OWNER, "R3D_CURVE_NAME_SUFFIX")
VERSION = read_define(OWNER, "R3D_CURVE_VERSION")
HEADER_FIELDS = (("COUNT", "H"), ("KNOT_COUNT", "H"), ("VERSION", "H"), ("RESERVED", "H"))
HEADER = checked_struct(OWNER, "R3D_CURVE", HEADER_FIELDS)  # the reserved field is zero
CURVE = struct.Struct("<2H6h")  # first knot, knot count, box lo xyz, box hi xyz
KNOT = struct.Struct("<3h2H")   # x y z, radius, panel RGB565
KNOT_POSITION = slice(0, 3)
CURVE_BOX = slice(2, 8)
