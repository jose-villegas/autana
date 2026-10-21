"""The physical look of a scene file: the scene without its `[indirect]` table and without `[bake].ao`.

    python physical_scene.py SCENE.scene.toml OUT.scene.toml [--look NAME]

The doc image studies of one option (indirect light, occlusion) start from this copy, so they isolate the option
and do not inherit the look the committed scene sets for its renders. --look then sets the one option a study
varies, from LOOKS. The copy lives in another folder, so a camera path's animation, named relative to the scene,
is made absolute."""
import argparse
import pathlib
import re
from typing import NamedTuple

ANIMATION = re.compile(r'(animation\s*=\s*")([^"]+)(")')


class Look(NamedTuple):
    """One study's change to the physical scene: no bounced light, a table before the first object, the
    ambient intensity, a `[bake]` line after its indirect line."""
    direct: bool = False
    table: str = ""
    ambient: float = None
    bake: str = ""


# The scene's ambient light is faint, so the occlusion has little to scale; both occlusion looks raise it.
OCCLUSION_AMBIENT = 0.25
LOOKS = {
    "smooth": Look(),
    "direct": Look(direct=True),
    "intensity-2": Look(table="[indirect]\nintensity = 2.0"),
    "intensity-3": Look(table="[indirect]\nintensity = 3.0"),
    "boost-2": Look(table="[indirect]\nalbedo_boost = 2.0"),
    "ao-flat": Look(ambient=OCCLUSION_AMBIENT),
    "ao-occluded": Look(ambient=OCCLUSION_AMBIENT, bake="ao = { distance = 80.0, rays = 32 }"),
}


def physical_scene(text, base=None, look=Look()):
    """`text` without its look, then with `look`; with `base`, the scene's folder, relative animation paths
    become absolute. A look whose option finds nothing to change in `text` is an error, not a copy that studies
    nothing."""
    if base is not None:
        text = ANIMATION.sub(lambda match: match.group(1) + (pathlib.Path(base) / match.group(2)).resolve().as_posix()
                             + match.group(3), text)
    out, table = [], None
    # Each place the look changes, and whether the look still has to find it.
    missing = {"[[objects]]": bool(look.table), "[bake] indirect": look.direct or bool(look.bake),
               "[ambient] intensity": look.ambient is not None}
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("["):
            table = stripped
        if stripped == "[[objects]]" and missing["[[objects]]"]:
            out.append(look.table + "\n\n")
            missing["[[objects]]"] = False
        if table == "[indirect]":
            continue
        if table == "[bake]" and re.match(r"ao\s*=", stripped):
            continue
        if table == "[bake]" and re.match(r"indirect\s*=", stripped):
            missing["[bake] indirect"] = False
            if look.direct:
                continue
            if look.bake:
                line += look.bake + "\n"
        if table == "[ambient]" and look.ambient is not None and re.match(r"intensity\s*=", stripped):
            line = f"intensity = {look.ambient}\n"
            missing["[ambient] intensity"] = False
        out.append(line)
    absent = [place for place, needed in missing.items() if needed]
    if absent:
        raise ValueError(f"the look changes {', '.join(absent)}, which the scene does not have")
    return "".join(out)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("--look", choices=LOOKS, default="smooth")
    args = parser.parse_args()
    text = args.scene.read_text(encoding="utf-8")
    with open(args.out, "w", encoding="utf-8", newline="") as target:
        target.write(physical_scene(text, args.scene.parent, LOOKS[args.look]))
