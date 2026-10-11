"""Names of animation settings and their pack entries."""

import pathlib

SUFFIX = ".anim.toml"


def clip_id(path):
    """The pack id of a .anim.toml: its stem."""
    return pathlib.Path(path).name.removesuffix(SUFFIX)
