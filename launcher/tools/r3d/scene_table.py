"""Writes a scene file's objects as the const r3d_scene_t table a scene reads.

Standard library only. The table names the mesh symbols the importer wrote
and, for a camera with a path, the track symbols tools/anim/bake_tracks.py
wrote under the path's `tracks` prefix.
"""

import pathlib


def real(value):
    """A C float literal."""
    text = f"{float(value):.9g}"
    return text + ("F" if "." in text or "e" in text else ".0F")


def vec3(values):
    return "{" + ", ".join(real(value) for value in values) + "}"


def transform(obj):
    return "{" + ", ".join(vec3(part) for part in (obj.position, obj.rotation, obj.scale)) + "}"


def table_symbol(scene):
    return scene.path.name.removesuffix(".scene.toml") + "_scene"


def out_directory(scene):
    """Where the table goes: with the meshes it names, which must share one folder."""
    directories = {item.settings.out_dir for item in scene.renderers}
    if len(directories) != 1:
        raise ValueError("a scene's meshes must be written to one output directory")
    return directories.pop()


def table_source(scene, banner):
    name = table_symbol(scene)
    renderers = [item.object for item in scene.renderers]
    lights = [item for item in scene.objects if item.kind == "light"]
    camera = scene.camera
    path = camera.component.path if camera else None
    lines = [banner, "", f'#include "{name}_generated.h"', ""]
    includes = sorted({f'{item.variant.name}_mesh_generated.h' for item in scene.renderers})
    if path:
        includes.append(f"{path.tracks}_tracks_generated.h")
    lines += [f'#include "{include}"' for include in includes] + [""]
    lines.append(f"static const r3d_scene_renderer_t {name}_renderers[] = {{")
    for item in scene.renderers:
        lines.append(f'    {{"{item.object.name}", {transform(item.object)}, &{item.variant.name}_mesh}},')
    lines += ["};", ""]
    if lights:
        lines.append(f"static const r3d_scene_light_t {name}_lights[] = {{")
        for item in lights:
            light = item.component
            lines.append(f'    {{"{item.name}", {transform(item)}, {vec3(light["color"])}, {real(light["intensity"])}, '
                         f'{real(light["disc_degrees"])}}},')
        lines += ["};", ""]
    if path:
        lines += [f"static const r3d_scene_path_t {name}_camera_path = {{&{path.tracks}_clip, "
                  f"&{path.tracks}_{path.node}_translation, &{path.tracks}_{path.node}_rotation}};", ""]
    if camera:
        component = camera.component
        lo, hi = component.region if component.region else ([0.0] * 3, [0.0] * 3)
        lines += [f"static const r3d_scene_camera_t {name}_camera = {{",
                  f'    "{camera.name}", {transform(camera)}, {real(component.half_fov_short_tan)}, {real(component.near_z)},',
                  f"    {'true' if component.region else 'false'}, {vec3(lo)}, {vec3(hi)}, "
                  f"{'&' + name + '_camera_path' if path else 'NULL'},", "};", ""]
    lines += [f"const r3d_scene_t {name} = {{",
              f"    {name}_renderers, {len(renderers)},",
              f"    {name + '_lights' if lights else 'NULL'}, {len(lights)},",
              f"    {'&' + name + '_camera' if camera else 'NULL'},", "};", ""]
    return "\n".join(lines)


def header_source(scene, banner):
    return "\n".join([banner, "", "#pragma once", "", '#include "render/r3d_scene.h"', "",
                      f"extern const r3d_scene_t {table_symbol(scene)};", ""])


def write_scene_table(scene, banner_lines):
    """Writes <scene>_scene_generated.c and .h; returns the paths."""
    banner = "/*\n" + "\n".join((" * " + line).rstrip() for line in banner_lines) + "\n */"
    directory = out_directory(scene)
    stem = table_symbol(scene) + "_generated"
    written = []
    for suffix, source in ((".c", table_source(scene, banner)), (".h", header_source(scene, banner))):
        path = pathlib.Path(directory) / (stem + suffix)
        path.write_text(source, newline="\n")
        written.append(path)
    return written
