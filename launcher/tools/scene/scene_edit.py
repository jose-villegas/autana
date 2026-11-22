"""Reads a scene's object placements and its cameras' key paths, and writes changed ones back into its .scene.toml
or a camera's .keys.toml, keeping every comment and the file's own formatting: only the values named change.

    python launcher/tools/scene/scene_edit.py read SCENE.scene.toml [-o OUT.json]
    python launcher/tools/scene/scene_edit.py set SCENE.scene.toml --set NAME.FIELD=X,Y,Z [--set ...]
    python launcher/tools/scene/scene_edit.py read-keys PATH.keys.toml [-o OUT.json]
    python launcher/tools/scene/scene_edit.py set-keys PATH.keys.toml --key FROM:T:EX,EY,EZ:LX,LY,LZ [--key ...]

`read` prints the objects as JSON (or writes them to OUT.json), each with its name, parent, component kind, whether it
is a baked renderer, why `set` would refuse to place it (null when it would not), and its position, rotation (Euler
degrees) and scale as authored, defaults filled in. `set` changes FIELD (position, rotation
or scale) of the object NAME; a key the object lacks is added. It writes .scene.toml files only, and places only
renderers that are not baked: a baked renderer is traced where it sits, at identity, and a light or a camera changes
the lighting every bake traces. It never adds, removes or reorders objects, so the cameras keep their order. The
result stays only when import_settings.load_scene accepts it; otherwise the file's bytes are put back, and it exits 1
and says why on stderr.

A camera's entry in `read` also has its `path`: null on no path, else the .anim.toml it flies, the .keys.toml that
clip is built from (null for a .glb clip, which `set-keys` cannot write), and `feeds_bakes`: true when a bake reads
the path (the scene camera's, kept by a camera_path visibility or posing a fit), so changing it re-keys those bakes.
`read-keys` prints a .keys.toml's node, animation and keys. `set-keys` takes the whole new list of keys in order, each
FROM the index of the key it was in the file, or `new`. A kept key keeps its table, the comment lines above it and its
layout, with only its changed numbers written; a key no FROM names goes, with the comment lines above it. Nothing is
written unless the result builds as anim/camera_keys.py builds a path.
"""

import argparse
import json
import math
import pathlib
import sys
import tomllib

import tomlkit

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from anim import camera_keys, tracks_asset  # noqa: E402
from bake import bake  # noqa: E402
from r3d import import_settings  # noqa: E402

SCENE_SUFFIX = ".scene.toml"
KEYS_SUFFIX = ".keys.toml"
KEYS_HEADER = "[[keys]]"
KEY_FIELDS = ("t", "eye", "look_at")
NEW_KEY = "new"
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
    """Every object's name, parent, kind, bake flag, lock reason and placement, in the file's order; a camera's
    path too."""
    scene = load(path)
    entries = []
    for item in scene.objects:
        entry = dict(name=item.name, parent=item.parent, kind=item.kind,
                     baked=item.kind in RENDERERS and bool(item.component.bake), locked=lock_reason(item),
                     position=item.position, rotation=item.rotation, scale=item.scale)
        if item.kind == "camera":
            entry["path"] = camera_path(item, scene)
        entries.append(entry)
    return entries


def camera_path(item, scene):
    """The camera `item`'s clip, the .keys.toml it is built from (None for another source), and whether a bake
    reads it; None on no path."""
    path = item.component.path
    if path is None:
        return None
    try:
        source, _ = tracks_asset.load_source(path.animation)
    except tracks_asset.TracksError as error:
        raise Refused(str(error)) from error
    keys = source.resolve() if source.name.endswith(KEYS_SUFFIX) else None
    return dict(anim=str(path.animation), keys=None if keys is None else str(keys),
                feeds_bakes=item is scene.camera and path_baked(scene))


def path_baked(scene):
    """Whether a bake reads the scene camera's path: a renderer keeps what it sees, or a fit is posed along it
    (the two uses import_settings.load_scene requires the path for)."""
    return any((job.renderer.visibility is not None and job.renderer.visibility.source == "camera_path")
               or (job.renderer.fit and job.settings.fit.probes is None) for job in scene.renderers)


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


def keys_path(text):
    """`text` as a path, refused unless it names a .keys.toml."""
    path = pathlib.Path(text)
    if not path.name.endswith(KEYS_SUFFIX):
        raise Refused(f"{path}: only a {KEYS_SUFFIX} is written, never its clip's other files")
    return path


def checked_keys(path, text):
    """The keys file text's values, refused unless camera_keys builds a path from them."""
    try:
        values = tomllib.loads(text)
        camera_keys.build(values)
    except (tomllib.TOMLDecodeError, ValueError, OverflowError) as error:
        raise Refused(f"{path}: {error}") from error
    return values


def read_text(path):
    """The file's text with its own line ends, its failure as Refused."""
    try:
        with open(path, encoding="utf-8", newline="") as source:
            return source.read()
    except OSError as error:
        raise Refused(f"{path}: {error}") from error


def read_keys(path):
    """The keys file's node, animation and keys, as authored."""
    values = checked_keys(path, read_text(path))
    return dict(node=values["node"], animation=values["animation"],
                keys=[{name: key[name] for name in KEY_FIELDS} for key in values["keys"]])


def plain(value):
    """`value` as the keys files write it: a whole number without a point."""
    return int(value) if value.is_integer() else value


def parse_key(text):
    """`FROM:T:EX,EY,EZ:LX,LY,LZ` as (FROM's index, or None for new; {t, eye, look_at})."""
    parts = text.split(":")
    if len(parts) != 4:
        raise Refused(f"--key {text!r}: expected FROM:T:EX,EY,EZ:LX,LY,LZ")
    origin, *values = parts
    if origin != NEW_KEY and not origin.isdigit():
        raise Refused(f"--key {text!r}: FROM is a key's index in the file, or {NEW_KEY}")
    try:
        numbers = [[float(value) for value in part.split(",")] for part in values]
    except ValueError as error:
        raise Refused(f"--key {text!r}: {error}") from error
    if [len(part) for part in numbers] != [1, AXES, AXES]:
        raise Refused(f"--key {text!r}: one time, then {AXES} numbers, comma-separated, for eye and for look_at")
    if not all(math.isfinite(value) for part in numbers for value in part):
        raise Refused(f"--key {text!r}: every number must be finite")
    t, eye, look_at = ([plain(value) for value in part] for part in numbers)
    return (None if origin == NEW_KEY else int(origin)), dict(t=t[0], eye=eye, look_at=look_at)


def key_tables(text):
    """`text` cut at its key tables: the text before them, each table as (the blank and comment lines above it,
    the table), and the blank and comment lines after the last. A comment above a table is that table's."""
    lines = text.splitlines(keepends=True)
    headers = [index for index, line in enumerate(lines) if line.split("#")[0].strip() == KEYS_HEADER]

    def above(end, start):
        """Where the run of blank and comment lines ending before line `end` starts, no earlier than `start`."""
        while end > start and (not lines[end - 1].strip() or lines[end - 1].lstrip().startswith("#")):
            end -= 1
        return end

    leads = [above(header, headers[index - 1] + 1 if index else 0) for index, header in enumerate(headers)]
    tail = above(len(lines), headers[-1] + 1)
    ends = leads[1:] + [tail]
    tables = [("".join(lines[leads[i]:headers[i]]), "".join(lines[headers[i]:ends[i]])) for i in range(len(headers))]
    return "".join(lines[:leads[0]]), tables, "".join(lines[tail:])


def edited_table(table, key):
    """The key table text `table` with `key`'s numbers written where they changed, element by element, so every
    comment and the layout stay."""
    document = tomlkit.parse(table)
    values = document["keys"][0]
    if values["t"] != key["t"]:
        old = values["t"].trivia
        item = tomlkit.item(key["t"])
        item.trivia.comment_ws, item.trivia.comment, item.trivia.trail = old.comment_ws, old.comment, old.trail
        values["t"] = item
    for name in ("eye", "look_at"):
        for index, value in enumerate(key[name]):
            if values[name][index] != value:
                values[name][index] = value
    return tomlkit.dumps(document)


def new_table(key, newline):
    """A key table of its own, in the keys files' layout."""
    def vector(values):
        return "[" + ", ".join(str(value) for value in values) + "]"
    lines = (KEYS_HEADER, f"t = {key['t']}", f"eye = {vector(key['eye'])}", f"look_at = {vector(key['look_at'])}")
    return "".join(line + newline for line in lines)


def apply_keys(path, keys):
    """Writes `keys` ((FROM or None, key) each, in order) as the whole list of the keys file at `path`, or nothing."""
    text = read_text(path)
    held = checked_keys(path, text)["keys"]
    origins = [origin for origin, _ in keys if origin is not None]
    for origin in origins:
        if origin >= len(held):
            raise Refused(f"{path}: no key {origin}; the file has {len(held)}")
        if origins.count(origin) > 1:
            raise Refused(f"{path}: key {origin} is named twice")
    newline = "\r\n" if "\r\n" in text else "\n"
    before, tables, after = key_tables(text)
    out = [before]
    for origin, key in keys:
        if origin is None:
            out.append(newline + new_table(key, newline))
        else:
            lead, table = tables[origin]
            out.append(lead + edited_table(table, key))
    written = "".join(out) + after
    if checked_keys(path, written)["keys"] != [key for _, key in keys]:
        raise Refused(f"{path}: the written file would not read back as the keys given")
    replace(path, written.encode("utf-8"))


def replace(path, data):
    """`path` holds `data`, all of it or none, through bake.place's write by rename."""
    try:
        bake.place(data, path)
    except bake.BakeMissing as error:
        raise Refused(str(error)) from error


def emit(text, out):
    """`text` printed, or written to the file `out` names."""
    if out:
        pathlib.Path(out).write_text(text + "\n", encoding="utf-8")
    else:
        print(text)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    read = commands.add_parser("read", help="print the objects' placements as JSON")
    read.add_argument("scene")
    read.add_argument("-o", dest="out", metavar="OUT.json", help="write the JSON here instead of printing it")
    write = commands.add_parser("set", help="change objects' placements in the file")
    write.add_argument("scene")
    write.add_argument("--set", dest="assignments", action="append", required=True, metavar="NAME.FIELD=X,Y,Z")
    read_keys_command = commands.add_parser("read-keys", help="print a camera keys file's keys as JSON")
    read_keys_command.add_argument("keys")
    read_keys_command.add_argument("-o", dest="out", metavar="OUT.json", help="write the JSON here instead of printing it")
    set_keys = commands.add_parser("set-keys", help="write a camera keys file's whole list of keys")
    set_keys.add_argument("keys")
    set_keys.add_argument("--key", dest="keys_given", action="append", required=True,
                          metavar="FROM:T:EX,EY,EZ:LX,LY,LZ")
    args = parser.parse_args(argv)
    try:
        if args.command == "read":
            emit(json.dumps({"objects": placements(scene_path(args.scene))}), args.out)
        elif args.command == "set":
            apply(scene_path(args.scene), [parse_assignment(text) for text in args.assignments])
        elif args.command == "read-keys":
            emit(json.dumps(read_keys(keys_path(args.keys))), args.out)
        else:
            path = keys_path(args.keys)
            apply_keys(path, [parse_key(text) for text in args.keys_given])
    except (Refused, OSError) as error:
        print(f"scene_edit: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
