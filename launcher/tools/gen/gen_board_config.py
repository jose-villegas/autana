#!/usr/bin/env python3
"""Write a board's board_config.h from its board.toml. Run from launcher/:
    python tools/gen/gen_board_config.py main/board/<vendor>/<board>/board.toml main/board/<vendor>/<board>/board_config.h

Each table's key becomes BOARD_<TABLE>_<KEY> (a top-level key, BOARD_<KEY>): an integer as written
in decimal, a float with an F suffix, a string as a C string, a number list as a brace list. A
`chip` names a driver's const table, so it is written bare, as a C symbol. Arrays of tables are
for tools and are not written. From the IMU's axes come BOARD_IMU_TO_SCREEN(x, y, z), which maps
chip axes to screen labels, and BOARD_IMU_UP_X/Y/Z, each chip axis's share of up the screen.
"""
import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "device"))

from board import chip_axes, load_board, screen_from_chip  # noqa: E402

COMMAND = "python tools/gen/gen_board_config.py"
CHIP_KEY = "chip"


def literal(key, value, where):
    if isinstance(value, bool):
        raise SystemExit(f"{where}: booleans have no board_config.h form")
    if isinstance(value, (int, float)):
        number = f"{value!r}F" if isinstance(value, float) else str(value)
        return f"({number})" if value < 0 else number
    if isinstance(value, str):
        if key == CHIP_KEY:
            if not value.isidentifier():
                raise SystemExit(f"{where}: a chip is a C symbol, not {value!r}")
            return value
        return '"' + value.replace("\\", "\\\\").replace('"', '\\"') + '"'
    if isinstance(value, list) and all(isinstance(item, (int, float)) and not isinstance(item, bool)
                                       for item in value):
        return "{" + ", ".join(literal(key, item, where) for item in value) + "}"
    raise SystemExit(f"{where}: {type(value).__name__} has no board_config.h form")


def defines(board):
    rows = []
    for table, values in board.items():
        if isinstance(values, dict):
            rows += [(f"BOARD_{table}_{key}".upper(), literal(key, value, f"{table}.{key}"))
                     for key, value in values.items()]
        elif not (isinstance(values, list) and values and all(isinstance(item, dict) for item in values)):
            rows.append((f"BOARD_{table}".upper(), literal(table, values, table)))
    return rows


def to_screen(board):
    """The screen-label vector for chip readings x, y, z, as a brace list over those arguments."""
    terms = []
    for row in screen_from_chip(board):
        parts = [("-" if weight < 0 else "") + f"({name})" for weight, name in zip(row, "xyz") if weight]
        terms.append(" + ".join(parts).replace("+ -", "- ") or "0")
    return "{" + ", ".join(terms) + "}"


def up(board):
    """BOARD_IMU_UP_X/Y/Z: how much of device +y, up the screen, each chip axis reads."""
    return [(f"BOARD_IMU_UP_{name}", literal(name, axis[1], "imu"))
            for name, axis in zip("XYZ", chip_axes(board))]


def render(board, source, output):
    rows = defines(board) + up(board)
    rows.append(("BOARD_IMU_TO_SCREEN(x, y, z)", to_screen(board)))
    names = [name.split("(")[0] for name, _ in rows]
    clashes = sorted({name for name in names if names.count(name) > 1})
    if clashes:
        raise SystemExit(f"{source}: more than one key writes {', '.join(clashes)}")
    width = max(len(name) for name, _ in rows)
    body = "".join(f"#define {name:<{width}} {value}\n" for name, value in rows)
    return ("/* GENERATED FILE - do not edit.\n"
            " *\n"
            f" *     {COMMAND} {source} {output}\n"
            " *\n"
            f" * board_config: {board['name']}, from {Path(source).name}.\n"
            " */\n"
            "#pragma once\n\n" + body)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("board", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    text = render(load_board(args.board), args.board.as_posix(), args.output.as_posix())
    args.output.write_text(text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
