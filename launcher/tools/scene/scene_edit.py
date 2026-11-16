"""Reads a scene's object placements, and writes changed ones back into its .scene.toml, keeping every comment and
the file's own formatting: only the values named change.

    python launcher/tools/scene/scene_edit.py read SCENE.scene.toml [-o OUT.json]
    python launcher/tools/scene/scene_edit.py set SCENE.scene.toml --set NAME.FIELD=X,Y,Z [--set ...]

`read` prints the objects as JSON (or writes them to OUT.json), each with its name, parent, component kind, whether it
is a baked renderer, why `set` would refuse to place it (null when it would not), and its position, rotation (Euler
degrees) and scale as authored, defaults filled in. `set` changes FIELD (position, rotation
or scale) of the object NAME; a key the object lacks is added. It writes .scene.toml files only, and places only
renderers that are not baked: a baked renderer is traced where it sits, at identity, and a light or a camera changes
the lighting every bake traces. It never adds, removes or reorders objects, so the cameras keep their order. The
result stays only when import_settings.load_scene accepts it; otherwise the file's bytes are put back, and it exits 1
and says why on stderr.
"""

import argparse
import json
import pathlib
import sys

import tomlkit

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from bake import bake  # noqa: E402
from r3d import import_settings  # noqa: E402

SCENE_SUFFIX = ".scene.toml"
FIELDS = ("position", "rotation", "scale")
RENDERERS = ("mesh_renderer", "skinned_renderer")
AXES = 3


class Refused(Exception):
    """An edit scene_edit will not make; its message says why."""


def scene_path(text):
    """`text` as a path, refused unless it names a .scene.toml."""
    path = pathlib.Path(text)
    if not path.name.endswith(SCENE_SUFFIX):
        raise Refused(f"{path}: only a {SCENE_SUFFIX} is written, never its meshes' sources")
    return path


def load(path):
    """import_settings' reading of the scene at `path`, its refusal as Refused."""
    try:
        return import_settings.load_scene(path)
    except (import_settings.SettingsError, OSError, ValueError) as error:
        raise Refused(f"{path}: {error}") from error


def placements(path):
    """Every object's name, parent, kind, bake flag, lock reason and placement, in the file's order."""
    return [dict(name=item.name, parent=item.parent, kind=item.kind,
                 baked=item.kind in RENDERERS and bool(item.component.bake), locked=lock_reason(item),
                 position=item.position, rotation=item.rotation, scale=item.scale) for item in load(path).objects]


def parse_assignment(text):
    """`NAME.FIELD=X,Y,Z` as (name, field, [x, y, z])."""
    target, equals, values = text.partition("=")
    name, dot, field = target.rpartition(".")
    if not equals or not dot or not name:
        raise Refused(f"--set {text!r}: expected NAME.FIELD=X,Y,Z")
    if field not in FIELDS:
        raise Refused(f"--set {text!r}: FIELD is one of {', '.join(FIELDS)}")
    try:
        vector = [float(value) for value in values.split(",")]
    except ValueError as error:
        raise Refused(f"--set {text!r}: {error}") from error
    if len(vector) != AXES:
        raise Refused(f"--set {text!r}: {AXES} numbers, comma-separated")
    return name, field, vector


def lock_reason(item):
    """Why a placement edit may not move `item` (a light, a camera, or a baked renderer); None when it may."""
    if item.kind not in RENDERERS:
        return f"{item.name!r} is a {item.kind}; only renderers that are not baked are placed"
    if item.component.bake:
        return f"{item.name!r} is baked (bake = true): a baked renderer is traced where it sits"
    return None


def check_placeable(item, path):
    """Refuses an object lock_reason names a reason for."""
    reason = lock_reason(item)
    if reason is not None:
        raise Refused(f"{path}: {reason}")


def set_value(table, field, vector):
    """Writes `vector` into `table`'s `field`: element by element when it is there, so its comment and layout
    stay; when it is not, as a line of its own in the scene format's order (name, parent, position, rotation,
    scale), ended as the line before it is."""
    if field in table:
        array = table[field]
        for index, value in enumerate(vector):
            array[index] = value
        return
    order = ("name", "parent", *FIELDS)
    before = [key for key in order[:order.index(field)] if key in table][-1]
    item = tomlkit.item(vector)
    item.trivia.trail = table[before].trivia.trail
    # Container.add would put a blank line before a sub-table that follows; inserting after a key adds nothing.
    table.value._insert_after(before, field, item)  # tomlkit is pinned exactly, so its private call holds


def apply(path, assignments):
    """Writes `assignments` ((name, field, vector) each) into the scene at `path`, all of them or none."""
    objects = {item.name: item for item in load(path).objects}
    with open(path, encoding="utf-8", newline="") as source:
        document = tomlkit.parse(source.read())
    tables = {str(table["name"]): table for table in document["objects"]}
    for name, field, vector in assignments:
        if name not in objects:
            raise Refused(f"{path}: no object {name!r}")
        check_placeable(objects[name], path)
        set_value(tables[name], field, vector)
    original = path.read_bytes()
    replace(path, tomlkit.dumps(document).encode("utf-8"))
    # Checked in place: the scene's id is its file name, and its asset names are checked against the pack's limit.
    try:
        import_settings.load_scene(path)
    except (import_settings.SettingsError, OSError, ValueError) as error:
        replace(path, original)
        raise Refused(f"{path}: {error}") from error


def replace(path, data):
    """`path` holds `data`, all of it or none, through bake.place's write by rename."""
    try:
        bake.place(data, path)
    except bake.BakeMissing as error:
        raise Refused(str(error)) from error


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    read = commands.add_parser("read", help="print the objects' placements as JSON")
    read.add_argument("scene")
    read.add_argument("-o", dest="out", metavar="OUT.json", help="write the JSON here instead of printing it")
    write = commands.add_parser("set", help="change objects' placements in the file")
    write.add_argument("scene")
    write.add_argument("--set", dest="assignments", action="append", required=True, metavar="NAME.FIELD=X,Y,Z")
    args = parser.parse_args(argv)
    try:
        path = scene_path(args.scene)
        if args.command == "read":
            text = json.dumps({"objects": placements(path)})
            if args.out:
                pathlib.Path(args.out).write_text(text + "\n", encoding="utf-8")
            else:
                print(text)
        else:
            apply(path, [parse_assignment(text) for text in args.assignments])
    except Refused as error:
        print(f"scene_edit: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
