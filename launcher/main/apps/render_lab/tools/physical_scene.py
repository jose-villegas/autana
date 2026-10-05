"""The physical look of a scene file: the scene without its `[indirect]` table and without `[bake].ao`.

    python physical_scene.py SCENE.scene.toml OUT.scene.toml

The doc image studies of one option (indirect light, occlusion) start from this copy, so they isolate the option
and do not inherit the look the committed scene sets for its renders."""
import re
import sys


def physical_scene(text):
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
        target.write(physical_scene(text))
