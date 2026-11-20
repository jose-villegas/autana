#!/usr/bin/env python3
"""Fail when a render setting has two owners or a settings file sets an undeclared key.

    python3 scripts/gates/check_render_settings.py           # check
    python3 scripts/gates/check_render_settings.py --table   # every render-related setting, as Markdown

A render setting (a render_settings_t field, nested structs included) has one
owner. A lens_t field named for its path joined by `_` or for its group, or a
NAME_set_X setter in a header under main/render or main/scene named for its
path, is a second override path. Every tracked *.render.toml sets only declared
fields, each within its interval. Where a setting belongs (its unit) is not
checked here; that stays a review rule.

--table lists those settings with the scene file's keys (tools/r3d/import_settings.py),
TUNE knobs in main/apps and app struct fields named like a render setting or lens field.
"""
import argparse
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
LAUNCHER = ROOT / "launcher"
sys.path.insert(0, str(LAUNCHER / "tools/render/settings"))
sys.path.insert(0, str(LAUNCHER / "tools"))
import render_asset  # noqa: E402

SETTINGS_HEADER = "main/render/settings/render_settings.h"
LENS_HEADER = "main/scene/camera/lens.h"
CAMERA_HEADER = "main/scene/camera/camera.h"
# Below these, every header's NAME_set_X setters are owners a render setting must not share.
OWNER_ROOTS = ("main/render", "main/scene")
CONTEXT_HEADER = "main/render/context/render_context.h"
SCENE_HEADER = "main/scene/scene.h"
# The scene file tables whose keys change what is drawn (load_scene's, the look's and the bake's).
SCENE_TABLES = ("scene", "scene.indirect", "scene.bake")
# The scene file keys that are tables, listed by their own keys.
SCENE_SUBTABLES = ("objects", "sky", "ambient", "indirect", "bake")
DOCS_SHOWN = 3


def text(launcher, path):
    return (launcher / path).read_text(encoding="utf-8")


def struct_fields(source, name):
    """[(C type, field)] of struct `name` in `source`, a `a, b` declaration as two."""
    body = re.search(r"typedef struct \{(.*?)\} " + name + ";", source, re.S).group(1)
    fields = []
    for line in body.splitlines():
        line = re.sub(r"/\*.*?\*/", "", line).strip().rstrip(";")
        if line and not line.startswith("//"):
            kind, _, names = line.partition(" ")
            fields += [(kind, n.strip()) for n in names.split(",")]
    return fields


def setters(source, prefix):
    return re.findall(rf"\w+ {prefix}(\w+)\(", source)


def setting_names(launcher=LAUNCHER):
    """({name: field}, {group: field}): each render setting's path joined by `_` (lod_blend),
    and each struct group's name (lod)."""
    names, groups = {}, {}
    for field in render_asset.fields(text(launcher, SETTINGS_HEADER)):
        names[field.replace(".", "_")] = field
        for group in field.split(".")[:-1]:
            groups.setdefault(group, field)
    return names, groups


def other_owners(launcher=LAUNCHER):
    """[(name, where)]: lens_t's fields, and every NAME_set_X setter declared in a header
    under OWNER_ROOTS as X."""
    found = [(name, f"lens_t.{name}") for _, name in struct_fields(text(launcher, LENS_HEADER), "lens_t")]
    for root in OWNER_ROOTS:
        for header in sorted((launcher / root).rglob("*.h")):
            for setter, name in re.findall(r"\w[\w *]* \**(\w+?_set_(\w+))\(", header.read_text(encoding="utf-8")):
                found.append((name, f"{setter}()"))
    return found


def git_files(pattern, where="."):
    return subprocess.run(["git", "ls-files", "--", pattern], cwd=where, capture_output=True,
                          text=True, check=True).stdout.split()


def problems(launcher=LAUNCHER, settings_files=None):
    found = []
    names, groups = setting_names(launcher)
    for name, where in other_owners(launcher):
        field = names.get(name) or (groups.get(name) if where.startswith("lens_t.") else None)
        if field is not None:
            found.append(f"{field} has two owners: render_settings_t and {where}")
    if settings_files is None:
        settings_files = [ROOT / p for p in git_files("*" + render_asset.SUFFIX, ROOT)]
    for path in settings_files:
        try:
            render_asset.read(path, text(launcher, SETTINGS_HEADER))
        except ValueError as error:
            found.append(str(error))
    return found


def grep_files(pattern, where):
    out = subprocess.run(["git", "grep", "-l", "-E", pattern, "--", where], cwd=LAUNCHER if where.startswith("main") else ROOT,
                         capture_output=True, text=True).stdout.split()
    return sorted(p for p in out if "/tests/" not in p)


def docs_naming(key):
    """The docs naming `key` (its last dotted parts) in backticks."""
    found = [pathlib.Path(p).stem for p in grep_files(rf"`\[?([a-z_]+\.)*{re.escape(key)}\]?( = [^`]*|\(\))?`", "docs")]
    return ", ".join(found[:DOCS_SHOWN]) + (" +" if len(found) > DOCS_SHOWN else "") or "none"


def callers(function, owner):
    """The files outside module `owner` that call `function`."""
    files = [p for p in grep_files(rf"\b{function}\(", "main") if not p.endswith(".h") and pathlib.Path(p).stem != owner]
    return ", ".join(pathlib.Path(p).stem for p in files) or "none"


def settings_rows():
    tuned = text(LAUNCHER, "main/render/settings/render_settings.c")
    for field, (kind, tier, _, _, _) in render_asset.fields().items():
        knob = re.escape(field.replace(".", "_")) + "|" + re.escape(field.split(".")[-1])
        dev = ", TUNE" if re.search(rf"TUNE_OVERRIDE\((render|anim), ({knob})\b", tuned) else ""
        yield ("render settings", field, kind, "render_settings_t", f"app pack ({tier}){dev}", docs_naming(field))
    declared = text(LAUNCHER, CAMERA_HEADER)
    for kind, name in struct_fields(text(LAUNCHER, LENS_HEADER), "lens_t"):
        setter = f"camera_set_{name}"
        set_by = callers(setter, "camera") if f"{setter}(" in declared else "scene file camera"
        yield ("camera", f"lens.{name}", kind, "lens_t", set_by, docs_naming(name))
    for kind, header, prefix, owner in (("render context", CONTEXT_HEADER, "render_context_set_", "render_context"),
                                        ("scene runtime", SCENE_HEADER, "scene_set_", "scene")):
        for name in setters(text(LAUNCHER, header), prefix):
            yield (kind, name, "", owner, callers(prefix + name, owner), docs_naming(prefix + name))


def scene_rows():
    """The scene file keys that change what is drawn: the top-level tables, the camera's and the lights'."""
    source = text(LAUNCHER, "tools/r3d/import_settings.py")
    calls = re.finditer(r'check_keys\((\w+), \(([^)]*)\), (f?"[^"]*")(?:, optional=\(([^)]*)\))?\)', source)
    for call in calls:
        where = call.group(3).strip('f"')
        if where in SCENE_TABLES:
            for key in re.findall(r'"(\w+)"', (call.group(2) or "") + (call.group(4) or "")):
                if key not in SCENE_SUBTABLES:
                    owner = "SCNE settings row" if where != "scene.bake" else "bake input (baked colours)"
                    yield ("scene file", f"{where}.{key}", "", owner, "scene.toml", docs_naming(key))
    camera = re.search(r"def load_camera\(.*?check_keys\(component, \(([^)]*)\), where, optional=\(([^)]*)\)", source, re.S)
    for key in re.findall(r'"(\w+)"', camera.group(1) + camera.group(2)):
        yield ("scene file", f"objects.camera.{key}", "", "SCNE camera row", "scene.toml", docs_naming(key))
    for kind, fields in re.findall(r'"(directional|sky|ambient)": \{([^}]*)\}', source):
        table = "objects.light" if kind == "directional" else f"scene.{kind}"
        for key in re.findall(r'"(\w+)":', fields):
            yield ("scene file", f"{table}.{key}", "", "SCNE light row", "scene.toml", docs_naming(table.split(".")[-1]))


def app_rows():
    """App struct fields named like a render setting or a lens field, and the apps' TUNE knobs."""
    engine = {*setting_names()[0], *(name for name, where in other_owners() if where.startswith("lens_t."))}
    for path in grep_files(r"typedef struct", "main/apps"):
        for body, name in re.findall(r"typedef struct \{(.*?)\} (\w+);", text(LAUNCHER, path), re.S):
            for field in re.findall(r"^\s*\w+ (\w+);", body, re.M):
                if field in engine:
                    yield ("app struct", f"{name}.{field}", "", pathlib.Path(path).stem, "app", docs_naming(field))
    for path in grep_files(r"TUNE_SETTING\(", "main/apps"):
        for owner, knob in re.findall(r"TUNE_SETTING\((\w+), (\w+),", text(LAUNCHER, path)):
            yield ("dev TUNE", f"{owner}.{knob}", "", pathlib.Path(path).stem, "TUNE only", docs_naming(knob))


def table():
    rows = [*settings_rows(), *scene_rows(), *app_rows()]
    print("| Kind | Setting | Type | Declared in | Set by | Docs naming it |")
    print("|---|---|---|---|---|---|")
    for row in rows:
        print("| " + " | ".join(f"`{c}`" if i == 1 else c for i, c in enumerate(row)) + " |")
    print(f"\n{len(rows)} rows")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--table", action="store_true", help="print every render-related setting as Markdown")
    if parser.parse_args(argv).table:
        return table()
    found = problems()
    for problem in found:
        print(problem)
    print(f"{len(found)} render setting problem(s)")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
