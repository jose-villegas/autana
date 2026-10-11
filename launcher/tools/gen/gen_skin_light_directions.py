#!/usr/bin/env python3
"""Generate unit directions for the runtime irradiance grid."""

import argparse
import math
from pathlib import Path
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    side = int(re.search(r"#define R3D_SKIN_LIGHT_TABLE_SIDE\s+(\d+)", args.header.read_text()).group(1))
    lines = ["/* GENERATED FILE - do not edit.",
             " * python tools/gen/gen_skin_light_directions.py main/render/skin/r3d_skin_light.h main/render/skin/r3d_skin_light_directions.h",
             " */", "#pragma once", '#include "render/skin/r3d_skin_light.h"',
             f'_Static_assert(R3D_SKIN_LIGHT_TABLE_SIDE == {side}, "regenerate irradiance directions");',
             "static const vec3f_t light_table_directions[R3D_SKIN_LIGHT_TABLE_CELLS] = {"]
    for y in range(side):
        for x in range(side):
            u, v = 2 * x / (side - 1) - 1, 2 * y / (side - 1) - 1
            z = 1 - abs(u) - abs(v)
            if z < 0:
                u, v = math.copysign(1 - abs(v), u), math.copysign(1 - abs(u), v)
            length = math.sqrt(u * u + v * v + z * z)
            values = ", ".join(f"{value / length:.9e}F" for value in (u, v, z))
            lines.append("    {" + values + "},")
    lines.append("};")
    args.output.write_bytes(("\n".join(lines) + "\n").encode())


if __name__ == "__main__":
    main()
