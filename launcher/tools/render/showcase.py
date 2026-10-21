"""Prints a scene file's showcase: its first renderer, the one pictures of the whole scene draw.

    python showcase.py SCENE.scene.toml"""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d.import_settings import load_scene  # noqa: E402


def showcase(scene):
    """The name of `scene`'s first renderer."""
    if not scene.renderers:
        raise ValueError("the scene has no renderer to show")
    return scene.renderers[0].object.name


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    print(showcase(load_scene(pathlib.Path(sys.argv[1]))))
