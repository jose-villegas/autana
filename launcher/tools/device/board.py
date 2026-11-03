"""The board a build compiles, read from its board.toml: main/board/<folder>/board.toml describes
the board's panel, chips, pins and shape once. gen_board_config.py writes board_config.h from it;
tools import PANEL_WIDTH and PANEL_HEIGHT, or load_board() for the rest."""

import sys
import tomllib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from build.packages import board_include_dir

BOARD_FILE = "board.toml"
CHIP_AXES = ("axis_x", "axis_y", "axis_z")
# Screen labels run x right, y down, z out; device axes x right, y up, z out. Callers read the
# chip's specific force, up at rest, as down, so x and z flip and y, flipped twice, does not.
SCREEN_FROM_DEVICE = (-1, 1, -1)


def load_board(path=None):
    """The board.toml at `path`, or the built board's."""
    with open(path or board_include_dir() / BOARD_FILE, "rb") as source:
        return tomllib.load(source)


def chip_axes(board):
    """The IMU chip's x, y, z axes as device vectors; each must be a signed device unit vector."""
    axes = [board["imu"][name] for name in CHIP_AXES]
    for name, axis in zip(CHIP_AXES, axes):
        if sorted(abs(value) for value in axis) != [0, 0, 1]:
            raise ValueError(f"imu.{name} = {axis} is not a signed device unit vector")
    if sorted(axis.index(next(v for v in axis if v)) for axis in axes) != [0, 1, 2]:
        raise ValueError("imu axes must each run along a different device axis")
    return axes


def screen_from_chip(board):
    """Per screen label x, y, z, the signed weight of chip x, y, z it reads: a 3x3 of -1, 0, 1."""
    axes = chip_axes(board)
    return [[SCREEN_FROM_DEVICE[label] * axes[chip][label] for chip in range(3)] for label in range(3)]


def screen_label_axes(board):
    """Per screen label x, y, z, the device vector the chip reading lands on."""
    axes = chip_axes(board)
    return [[sum(weight * axes[chip][i] for chip, weight in enumerate(row)) for i in range(3)]
            for row in screen_from_chip(board)]


def __getattr__(name):
    """PANEL_WIDTH and PANEL_HEIGHT, read from the built board on first use, so importing this
    module for another board reads nothing."""
    panel = {"PANEL_WIDTH": "width", "PANEL_HEIGHT": "height"}
    if name not in panel:
        raise AttributeError(name)
    return load_board()["panel"][panel[name]]
