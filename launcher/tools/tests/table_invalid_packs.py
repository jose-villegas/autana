#!/usr/bin/env python3
"""Invalid curve table packs for the host ridge fallback suite.

    python launcher/tools/tests/table_invalid_packs.py -o DIR
"""

import argparse
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

from asset import table_asset
from asset.asset_pack import build_pack
from gfx import curve_asset


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("-o", type=pathlib.Path, required=True)
    out = parser.parse_args().o
    columns, shift = curve_asset.VIEW_W, curve_asset.Q_SHIFT
    for case, count, frac, name in (("frac-bits", columns, shift + 1, curve_asset.ROW),
                                    ("columns", columns - 1, shift, curve_asset.ROW),
                                    ("row", columns, shift, "absent")):
        folder = out / ("ridge-invalid-" + case)
        folder.mkdir(parents=True, exist_ok=True)
        table = table_asset.encode({name: [x << shift for x in range(count)]}, frac)
        (folder / "engine.apak").write_bytes(build_pack([("ridge", table_asset.TYPE, table)]))


if __name__ == "__main__":
    main()
