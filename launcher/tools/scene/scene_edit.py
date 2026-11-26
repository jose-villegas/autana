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

A camera's entry in `read` also has its `path`: null on no path, else
  - `anim`: the .anim.toml the camera flies;
  - `keys`: the .keys.toml that clip is built from, or null for a .glb clip, which `set-keys` cannot write;
  - `feeds_bakes`: true when a bake reads the path (a renderer keeps what it sees along it, or a fit is posed along
    it), so changing it re-keys those bakes.

`read-keys` prints a .keys.toml's node, animation and keys. `set-keys` takes the whole new list of keys in order, each
FROM the index of the key it was in the file, or `new`. A kept key keeps its table, the comment lines above it and its
layout, with only its changed numbers written; a key no FROM names goes, with the comment lines above it. Nothing is
written unless the result builds as tools/anim/camera_keys.py builds a path.
"""

import argparse
import json
import math
import pathlib
import re
import sys
import tomllib

import tomlkit

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from anim import camera_keys, tracks_asset  # noqa: E402
from bake import bake  # noqa: E402
from gltf.gltf_read import KEYS_SUFFIX  # noqa: E402
from r3d import import_settings  # noqa: E402

SCENE_SUFFIX = ".scene.toml"
KEYS_HEADER = "[[keys]]"
# A [[keys]] header line, its key bare or quoted. A line inside a multi-line string can look like one: the
# headers found must then match the parsed keys, or the write is refused.
KEYS_HEADER_LINE = re.compile(r"""\s*\[\[\s*(keys|"keys"|'keys')\s*\]\]\s*(#.*)?""")
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
    reads it; None on no path. A clip file that does not read refuses the scene, though load_scene reads only its
    name."""
    path = item.component.path
    if path is None:
        return None
    try:
        source, _ = tracks_asset.load_source(path.animation)
    except tracks_asset.TracksError as error:
        raise Refused(str(error)) from error
    keys = source.resolve() if is_keys_file(source) else None
    return dict(anim=str(path.animation), keys=None if keys is None else str(keys),
                feeds_bakes=item is scene.camera and path_baked(scene))


def path_baked(scene):
    """Whether a bake reads the scene camera's path: a renderer keeps what it sees along it, or a fit takes its
    reference views from it, probes or not (bake.reference_recipe samples a path whenever there is one). Tests
    pin this to the poses bake.stage_keys asks for on every scene in the tree."""
    return any((job.renderer.visibility is not None and job.renderer.visibility.source == "camera_path")
               or job.renderer.fit is not None for job in scene.renderers)


def parse_assignment(text):
    """`NAME.FIELD=X,Y,Z` as (name, field, [x, y, z])."""
    target, equals, values = text.partition("=")
    name, dot, field = target.rpartition(".")
    if not equals or not dot or not name:
        raise Refused(f"--set {text!r}: expected NAME.FIELD=X,Y,Z")
    if field not in FIELDS:
        raise Refused(f"--set {text!r}: FIELD is one of {', '.join(FIELDS)}")
    return name, field, numbers(values, AXES, f"--set {text!r}")


def numbers(text, count, where):
    """`text`'s `count` comma-separated finite numbers, refused naming `where` otherwise."""
    try:
        values = [float(value) for value in text.split(",")]
    except ValueError as error:
        raise Refused(f"{where}: {error}") from error
    if len(values) != count:
        raise Refused(f"{where}: {count} numbers, comma-separated")
    if not all(math.isfinite(value) for value in values):
        raise Refused(f"{where}: every number must be finite")
    return values


def write_changed(array, values):
    """`values` into the TOML array `array`, element by element and only where they differ, so its comments, its
    layout and the spelling of every unchanged number stay."""
    for index, value in enumerate(values):
        if array[index] != value:
            array[index] = value


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
        write_changed(table[field], vector)
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
    document = tomlkit.parse(read_text(path))
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


def is_keys_file(path):
    """Whether `path` names a camera keys file, its suffix in any case, as gltf_read finds one."""
    return path.name.lower().endswith(KEYS_SUFFIX)


def keys_path(text):
    """`text` as a path, refused unless it names a .keys.toml."""
    path = pathlib.Path(text)
    if not is_keys_file(path):
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
    if origin != NEW_KEY and not (origin.isascii() and origin.isdigit()):
        raise Refused(f"--key {text!r}: FROM is a key's index in the file, or {NEW_KEY}")
    where = f"--key {text!r}"
    t, eye, look_at = ([plain(value) for value in numbers(part, count, where)]
                       for part, count in zip(values, (1, AXES, AXES)))
    return (None if origin == NEW_KEY else int(origin)), dict(t=t[0], eye=eye, look_at=look_at)


def key_tables(text):
    """(the text before the first key's comments, [(the blank and comment lines above a key, its table)], the
    trailing text). A comment above a table belongs to that table. None when the keys are not [[keys]] tables."""
    lines = text.splitlines(keepends=True)
    headers = [index for index, line in enumerate(lines) if KEYS_HEADER_LINE.fullmatch(line.rstrip("\r\n"))]
    if not headers:
        return None

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
        values["t"] = key["t"]  # tomlkit keeps the line's comment
    for name in ("eye", "look_at"):
        write_changed(values[name], key[name])
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
    cut = key_tables(text)
    if cut is None:
        raise Refused(f"{path}: keys are written only as {KEYS_HEADER} tables, one per key")
    before, tables, after = cut
    if len(tables) != len(held):
        raise Refused(f"{path}: {len(held)} keys but {len(tables)} {KEYS_HEADER} tables; keys are written only as "
                      "tables, one per key")
    newline = "\r\n" if "\r\n" in text else "\n"
    written = before
    for origin, key in keys:
        if written and not written.endswith("\n"):
            written += newline  # a last table the file ended without a line end
        if origin is None:
            written += newline + new_table(key, newline)
        else:
            lead, table = tables[origin]
            written += lead + edited_table(table, key)
    written += after
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


def add_reader(commands, name, file, help_text):
    """A command that prints what `file` holds as JSON, or writes it to -o."""
    reader = commands.add_parser(name, help=help_text)
    reader.add_argument(file)
    reader.add_argument("-o", dest="out", metavar="OUT.json", help="write the JSON here instead of printing it")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    commands = parser.add_subparsers(dest="command", required=True)
    add_reader(commands, "read", "scene", "print the objects' placements as JSON")
    write = commands.add_parser("set", help="change objects' placements in the file")
    write.add_argument("scene")
    write.add_argument("--set", dest="assignments", action="append", required=True, metavar="NAME.FIELD=X,Y,Z")
    add_reader(commands, "read-keys", "keys", "print a camera keys file's keys as JSON")
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
