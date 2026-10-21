"""Export a model's meshes and base-colour textures to an OBJ, inside Blender.

    blender --background --factory-startup --python launcher/tools/r3d/source/blend_export_obj.py -- \
        IN OUT_DIR --name NAME [--texture-size PX] [--objects PATTERN] [--source-up Z|Y]

reduce_source.py runs this; see there. Imports IN (.fbx, .obj, .glb/.gltf or
.blend), keeps the mesh objects whose names match --objects (all when left
out), and writes OUT_DIR/NAME.obj, triangulated, with its .mtl and each
base-colour texture, at most PX on its longer side, as a PNG under
OUT_DIR/textures/; other textures are dropped, since a bake reads only albedo
and opacity. Positions are written in the source's units, +Y up and looking
down -Z, as the importer reads them. Blender's importers turn a source to +Z
up; one whose file misstates its axes stays +Y up there, and --source-up Y
writes it unturned. The input is only read.
"""
import argparse
import fnmatch
import os
import sys

import bpy

# The OBJ exporter's (forward_axis, up_axis) for each up axis of the imported
# scene, so the OBJ is +Y up either way.
UP_AXES = {"Z": ("NEGATIVE_Z", "Y"), "Y": ("Y", "Z")}
# The Principled BSDF inputs whose textures the importer reads.
ALBEDO_INPUTS = ("Base Color", "Alpha")


def arguments():
    parser = argparse.ArgumentParser(prog="blend_export_obj.py", description=__doc__.split("\n\n")[0])
    parser.add_argument("source")
    parser.add_argument("out_dir")
    parser.add_argument("--name", required=True, help="the OBJ's file name, without extension")
    parser.add_argument("--texture-size", type=int, default=512, help="the longest texture side, in pixels")
    parser.add_argument("--objects", default="*", help="keep mesh objects whose names match this glob")
    parser.add_argument("--source-up", choices=UP_AXES, default="Z", help="the up axis as Blender read the source")
    return parser.parse_args(sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else [])


def load(path):
    extension = os.path.splitext(path)[1].lower()
    if extension == ".blend":
        bpy.ops.wm.open_mainfile(filepath=path, load_ui=False, use_scripts=False)
    elif extension == ".fbx":
        bpy.ops.import_scene.fbx(filepath=path)
    elif extension == ".obj":
        bpy.ops.wm.obj_import(filepath=path)
    elif extension in (".glb", ".gltf"):
        bpy.ops.import_scene.gltf(filepath=path)
    else:
        sys.exit(f"{path}: cannot read {extension or 'a file without an extension'}")


def kept_meshes(pattern):
    meshes = [obj for obj in bpy.data.objects if obj.type == "MESH" and fnmatch.fnmatchcase(obj.name, pattern)]
    for obj in bpy.data.objects:
        if obj not in meshes:
            bpy.data.objects.remove(obj, do_unlink=True)
    if not meshes:
        sys.exit(f"no mesh object matches {pattern}")
    return meshes


def keep_albedo_textures():
    """Removes every image node that does not feed a material's base colour or
    alpha, so the OBJ's MTL names only textures the importer reads."""
    for material in bpy.data.materials:
        if material.node_tree is None:
            continue
        tree = material.node_tree
        feeding = {link.from_node for link in tree.links
                   if link.to_node.type == "BSDF_PRINCIPLED" and link.to_socket.name in ALBEDO_INPUTS}
        for node in [n for n in tree.nodes if n.type == "TEX_IMAGE" and n not in feeding]:
            tree.nodes.remove(node)


def shrink_textures(out_dir, size):
    """Every image a material still reads, saved as a PNG under out_dir/textures
    at most `size` on its longer side."""
    directory = os.path.join(out_dir, "textures")
    os.makedirs(directory, exist_ok=True)
    used = {node.image for material in bpy.data.materials if material.node_tree is not None
            for node in material.node_tree.nodes if node.type == "TEX_IMAGE" and node.image}
    for image in used:
        if image.size[0] == 0:
            continue
        width, height = image.size
        scale = min(1.0, size / max(width, height))
        if scale < 1.0:
            image.scale(max(1, round(width * scale)), max(1, round(height * scale)))
        stem = os.path.splitext(bpy.path.basename(image.filepath) or image.name)[0]
        image.filepath_raw = os.path.join(directory, stem + ".png")
        image.file_format = "PNG"
        image.save()


def tidy_mtl(path):
    """Drops a map_d that names its material's own map_Kd, since the importer
    reads map_d as a grey mask and the alpha already rides in map_Kd, and any
    texture Blender could not read and so did not save."""
    root = os.path.dirname(path)
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    out, albedo = [], None
    for line in lines:
        key, _, value = line.partition(" ")
        if key == "newmtl":
            albedo = None
        if key in ("map_Kd", "map_d"):
            if not os.path.isfile(os.path.join(root, value)):
                print(f"blend_export_obj: {value} was not saved; its material keeps no texture")
                continue
            if key == "map_d" and value == albedo:
                continue
            if key == "map_Kd":
                albedo = value
        out.append(line)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")


def main():
    args = arguments()
    bpy.ops.wm.read_factory_settings(use_empty=True)
    load(os.path.abspath(args.source))
    kept_meshes(args.objects)
    args.out_dir = os.path.abspath(args.out_dir)
    os.makedirs(args.out_dir, exist_ok=True)
    keep_albedo_textures()
    shrink_textures(args.out_dir, args.texture_size)
    forward, up = UP_AXES[args.source_up]
    bpy.ops.wm.obj_export(filepath=os.path.join(args.out_dir, args.name + ".obj"), apply_modifiers=False,
                          export_selected_objects=False, export_uv=True, export_normals=False,
                          export_materials=True, path_mode="RELATIVE", export_triangulated_mesh=True,
                          forward_axis=forward, up_axis=up)
    tidy_mtl(os.path.join(args.out_dir, args.name + ".mtl"))
    print(f"blend_export_obj: wrote {args.name}.obj")


main()
