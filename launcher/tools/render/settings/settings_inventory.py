#!/usr/bin/env python3
"""Every render-related setting, read from where each is declared, with who
sets it and which docs name it. Design aid for the render settings layout
draft (docs/plans/Render-Settings-Layout-Design-Sketch.md); prints a Markdown table.

    python launcher/tools/render/settings/settings_inventory.py

Sources: render_settings_t (render_settings.h), lens_t (scene/camera/lens.h),
the scene file's keys (tools/r3d/import_settings.py: its check_keys calls and
LIGHT_FIELDS), the render context's and the scene's setters, TUNE knobs in
main/apps, and render_lab's per-scene table (baked_families.h).
"""
import pathlib
import re
import subprocess
import sys

LAUNCHER = pathlib.Path(__file__).resolve().parents[3]
ROOT = LAUNCHER.parent
sys.path.insert(0, str(LAUNCHER / "tools/render/settings"))
sys.path.insert(0, str(LAUNCHER / "tools"))
import render_asset  # noqa: E402

# The scene file tables whose keys change what is drawn (load_scene's, the look's and the bake's).
SCENE_TABLES = ("scene", "scene.indirect", "scene.bake")
# The scene file keys that are tables, listed by their own keys.
SCENE_SUBTABLES = ("objects", "sky", "ambient", "indirect", "bake")
# render_context_set_* that wire a frame rather than choose how it is drawn.
CONTEXT_PLUMBING = ("_take", "_observer", "_changed", "_batches", "_settings")
DOCS_SHOWN = 3


def text(path):
    return (LAUNCHER / path).read_text(encoding="utf-8")


def struct_fields(path, name):
    """[(C type, field)] of `name` in `path`, a `a, b` declaration as two."""
    body = re.search(r"typedef struct \{(.*?)\} " + name + ";", text(path), re.S).group(1)
    fields = []
    for line in body.splitlines():
        line = re.sub(r"/\*.*?\*/", "", line).strip().rstrip(";")
        if line:
            kind, _, names = line.partition(" ")
            fields += [(kind, n.strip()) for n in names.split(",")]
    return fields


def git_files(pattern, where):
    out = subprocess.run(["git", "grep", "-l", "-E", pattern, "--", where], cwd=LAUNCHER if where == "main" else ROOT,
                         capture_output=True, text=True).stdout.split()
    return sorted(p for p in out if "/tests/" not in p)


def docs_naming(key):
    """The docs naming `key` (its last dotted parts) in backticks."""
    found = [pathlib.Path(p).stem for p in git_files(rf"`\[?([a-z_]+\.)*{re.escape(key)}\]?( = [^`]*|\(\))?`", "docs")]
    return ", ".join(found[:DOCS_SHOWN]) + (" +" if len(found) > DOCS_SHOWN else "") or "none"


def callers(function, owner):
    """The files outside module `owner` that call `function`."""
    files = [p for p in git_files(rf"\b{function}\(", "main") if not p.endswith(".h") and pathlib.Path(p).stem != owner]
    return ", ".join(pathlib.Path(p).stem for p in files) or "none"


def render_settings_rows():
    tuned = text("main/render/settings/render_settings.c")
    for field, (kind, tier, _) in render_asset.fields().items():
        knob = re.escape(field.replace(".", "_")) + "|" + re.escape(field.split(".")[-1])
        dev = ", TUNE" if re.search(rf"TUNE_OVERRIDE\((render|anim), ({knob})\b", tuned) else ""
        yield ("render settings", field, kind, "render_settings_t", f"app pack ({tier}){dev}", docs_naming(field))


def lens_rows():
    declared = text("main/scene/camera/camera.h")
    for kind, name in struct_fields("main/scene/camera/lens.h", "lens_t"):
        setter = f"camera_set_{name}"
        set_by = callers(setter, "camera") if f"{setter}(" in declared else "scene file camera"
        yield ("camera", f"lens.{name}", kind, "lens_t", set_by, docs_naming(name))


def scene_rows():
    """The scene file keys that change what is drawn: the top-level tables, the camera's and the lights'."""
    source = text("tools/r3d/import_settings.py")
    calls = re.finditer(r'check_keys\((\w+), \(([^)]*)\), (f?"[^"]*")(?:, optional=\(([^)]*)\))?\)', source)
    for call in calls:
        where = call.group(3).strip('f"')
        if where not in SCENE_TABLES:
            continue
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


def setter_rows(kind, header, prefix, owner, plumbing=()):
    for name in re.findall(rf"\w+ ({prefix}\w+)\(", text(header)):
        if not name.endswith(plumbing):
            yield (kind, name.removeprefix(prefix), "", owner, callers(name, owner), docs_naming(name))


def app_table_rows():
    for kind, name in struct_fields("main/apps/render_lab/baked_families.h", "scene_baked_t"):
        if kind == "bool":
            yield ("app C table", f"scene_baked_t.{name}", kind, "render_lab", "baked_families.c", docs_naming(name))


def tune_rows():
    for path in git_files(r"TUNE_SETTING\(", "main"):
        if "/apps/" in path:
            for owner, knob in re.findall(r"TUNE_SETTING\((\w+), (\w+),", (LAUNCHER / path).read_text(encoding="utf-8")):
                yield ("dev TUNE", f"{owner}.{knob}", "", pathlib.Path(path).stem, "TUNE only", docs_naming(knob))


def main():
    rows = [*render_settings_rows(), *lens_rows(), *scene_rows(),
            *setter_rows("render context", "main/render/context/render_context.h", "render_context_set_",
                         "render_context", CONTEXT_PLUMBING),
            *setter_rows("scene runtime", "main/scene/scene.h", "scene_set_", "scene"),
            *app_table_rows(), *tune_rows()]
    print("| Kind | Setting | Type | Declared in | Set by | Docs naming it |")
    print("|---|---|---|---|---|---|")
    for row in rows:
        print("| " + " | ".join(f"`{c}`" if i == 1 else c for i, c in enumerate(row)) + " |")
    print(f"\n{len(rows)} rows")


if __name__ == "__main__":
    main()
