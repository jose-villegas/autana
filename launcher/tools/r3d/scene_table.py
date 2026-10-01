#!/usr/bin/env python3
"""Write a scene file's table: what a scene reads at run time.

    python launcher/tools/r3d/scene_table.py SCENE.scene.toml

Standard library only, and independent of baking: it reads the scene file and
its import files and writes <scene>_scene_generated.c and .h beside the meshes.
The table holds only what the device reads: one const r3d_instance_t per mesh
renderer, named <scene>_scene_<object>, so a misspelt object fails at link
time, and the camera's lens, placement and path. A mesh is named by its asset
id, not linked: <scene>_scene_assets lists each id with the view
r3d_scene_bind() fills from the pack, and a missing id fails there. Lights, the camera region and
the tone map are bake settings and stay offline. The placements are baked as a
3x3 (rotation times scale) and a position, so the device does no trigonometry.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.import_settings import SettingsError, load_scene  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]


def real(value):
    """A C float literal."""
    text = f"{float(value) + 0.0:.9g}"
    return text + ("F" if "." in text or "e" in text else ".0F")


def vec3(values):
    return "{" + ", ".join(real(value) for value in values) + "}"


def placement(obj):
    """A C initializer for the object's baked placement, or None when it is the identity."""
    if obj.identity:
        return None
    return "{.m = {" + ", ".join(vec3(row) for row in obj.matrix) + "}, .position = " + vec3(obj.position) + "}"


def table_symbol(scene):
    return scene.path.name.removesuffix(".scene.toml") + "_scene"


def out_directory(scene):
    """Where the table goes: with the meshes it names, which must share one folder."""
    directories = {item.settings.out_dir for item in scene.renderers}
    if len(directories) != 1:
        raise ValueError("a scene's meshes must be written to one output directory")
    return directories.pop()


def banner_for(scene):
    try:
        relative = scene.path.relative_to(REPO).as_posix()
    except ValueError:  # a scene outside the repository, as a test builds one
        relative = scene.path.name
    lines = ["GENERATED FILE - do not edit.", "", f"    python launcher/tools/r3d/scene_table.py {relative}"]
    return "/*\n" + "\n".join((" * " + line).rstrip() for line in lines) + "\n */"


def statics(scene, obj, label, lines):
    """The object's placement as a static, or NULL; returns what refers to it."""
    text = placement(obj)
    if text is None:
        return "NULL"
    name = f"{table_symbol(scene)}_{label}_placement"
    lines += [f"static const r3d_placement_t {name} = {text};", ""]
    return "&" + name


def table_source(scene, banner):
    name = table_symbol(scene)
    camera = scene.camera
    path = camera.component.path if camera else None
    lines = [banner, "", "#include <stddef.h>", "", f'#include "{name}_generated.h"', ""]
    if path:
        lines += [f'#include "{path.tracks}_tracks_generated.h"', ""]
    for item in scene.renderers:
        obj = item.object
        refer = statics(scene, obj, obj.name, lines)
        lines += [f"static r3d_lit_mesh_t {name}_{obj.name}_mesh;",
                  f"const r3d_instance_t {name}_{obj.name} = {{.mesh = &{name}_{obj.name}_mesh, .placement = {refer}}};", ""]
    lines += [f"static const r3d_scene_mesh_t {name}_meshes[] = {{"]
    lines += [f'    {{"{item.variant.name}", &{name}_{item.object.name}_mesh}},' for item in scene.renderers]
    lines += ["};", "", f"const r3d_scene_assets_t {name}_assets = {{.meshes = {name}_meshes, "
              f".count = (int)(sizeof {name}_meshes / sizeof {name}_meshes[0])}};", ""]
    if camera:
        refer = statics(scene, camera, camera.name, lines)
        component = camera.component
        if path:
            lines += [f"static const r3d_scene_path_t {name}_{camera.name}_path = {{.clip = &{path.tracks}_clip, "
                      f".translation = &{path.tracks}_{path.node}_translation, "
                      f".rotation = &{path.tracks}_{path.node}_rotation}};", ""]
        lines += [f"const r3d_scene_camera_t {name}_{camera.name} = {{.half_fov_short_tan = {real(component.half_fov_short_tan)}, "
                  f".near_z = {real(component.near_z)}, .placement = {refer}, "
                  f".path = {'&' + name + '_' + camera.name + '_path' if path else 'NULL'}}};", ""]
    return "\n".join(lines)


def header_source(scene, banner):
    name = table_symbol(scene)
    lines = [banner, "", "#pragma once", "", '#include "render/r3d_scene.h"', ""]
    lines += [f"extern const r3d_instance_t {name}_{item.object.name};" for item in scene.renderers]
    lines.append(f"extern const r3d_scene_assets_t {name}_assets;")
    if scene.camera:
        lines.append(f"extern const r3d_scene_camera_t {name}_{scene.camera.name};")
    return "\n".join(lines) + "\n"


def table_files(scene):
    """(path, text) of the table's two files."""
    banner = banner_for(scene)
    stem = pathlib.Path(out_directory(scene)) / (table_symbol(scene) + "_generated")
    return [(stem.with_suffix(".c"), table_source(scene, banner)), (stem.with_suffix(".h"), header_source(scene, banner))]


def write_scene_table(scene):
    """Writes the table beside the meshes; returns the paths."""
    written = []
    for path, source in table_files(scene):
        path.write_text(source, newline="\n")
        written.append(path)
    return written


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene", help="a .scene.toml file")
    args = parser.parse_args(argv)
    try:
        scene = load_scene(args.scene)
        for path in write_scene_table(scene):
            print(f"wrote {path.name}")
    except (SettingsError, ValueError) as error:
        parser.error(str(error))
    return 0


if __name__ == "__main__":
    sys.exit(main())
