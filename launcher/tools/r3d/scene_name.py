"""Names of scene settings and their pack entries."""

import pathlib

SUFFIX = ".scene.toml"


def scene_id(path):
    """The pack id of a .scene.toml: its stem."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)
