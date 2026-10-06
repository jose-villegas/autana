"""The physical look of a scene file: the scene without its `[indirect]` table and without `[bake].ao`.

    python physical_scene.py SCENE.scene.toml OUT.scene.toml

The doc image studies of one option (indirect light, occlusion) start from this copy, so they isolate the option
and do not inherit the look the committed scene sets for its renders. The copy lives in another folder, so a camera
path's animation, named relative to the scene, is made absolute."""
import pathlib
import re
import sys

ANIMATION = re.compile(r'(animation\s*=\s*")([^"]+)(")')


def physical_scene(text, base=None):
    """`text` without its look; with `base`, the scene's folder, relative animation paths become absolute."""
    if base is not None:
        text = ANIMATION.sub(lambda match: match.group(1) + (pathlib.Path(base) / match.group(2)).resolve().as_posix()
                             + match.group(3), text)
    out, table = [], None
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("["):
            table = stripped
        if table == "[indirect]":
            continue
        if table == "[bake]" and re.match(r"ao\s*=", stripped):
            continue
        out.append(line)
    return "".join(out)


if __name__ == "__main__":
    with open(sys.argv[1], encoding="utf-8") as source:
        text = source.read()
    with open(sys.argv[2], "w", encoding="utf-8", newline="") as target:
        target.write(physical_scene(text, pathlib.Path(sys.argv[1]).parent))
