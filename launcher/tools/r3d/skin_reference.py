#!/usr/bin/env python3
"""Path trace a scene with a posed glTF skin, writing RGB, linear RGB and skin coverage."""
import argparse
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from r3d import mitsuba_reference as mr
from r3d.gltf_skin import SkinnedAsset, load_asset, mat_from_trs, transform_point
from r3d.import_settings import load_scene
from r3d.reference_render import device_picture, source_for

PIXELS_PER_PASS = 1 << 22
MASK_SPP = 16
MASK_THRESHOLD = 0.5
BYTE_MAX = 255
REFERENCE_PICTURE = "reference.png"
REFERENCE_LINEAR = "reference.linear.npy"
SKIN_COVERAGE = "skin.mask.npy"
SKIN_COVERAGE_PICTURE = "skin.mask.png"


def posed_skin(asset, clip, time, at, heading_deg, scale, white=False):
    """World positions, linear reflectances and triangles for one sampled skin."""
    duration = asset.duration(clip) if clip is not None else 0
    positions, _ = asset.skin(asset.sample(clip, time % duration if duration > 0 else 0))
    half = np.radians(heading_deg) / 2
    placement = mat_from_trs(at, (0, np.sin(half), 0, np.cos(half)), (scale,) * 3)
    world = np.array([transform_point(placement, p) for p in positions], dtype=np.float64)
    colors = np.ones_like(world) if white or asset.colors is None else np.asarray(asset.colors, float)[:, :3]
    return world, colors, np.asarray(asset.triangles, dtype=np.uint32)


def skin_shape(mi, world, colors, triangles):
    """A two-sided diffuse mesh with the glTF's linear vertex reflectances."""
    props = mi.Properties()
    props["bsdf"] = mi.load_dict({"type": "twosided", "bsdf": {
        "type": "diffuse", "reflectance": {"type": "mesh_attribute", "name": "vertex_color"}}})
    mesh = mi.Mesh("skin", len(world), len(triangles), props,
                   has_vertex_normals=False, has_vertex_texcoords=False)
    params = mi.traverse(mesh)
    params["vertex_positions"] = world.astype(np.float32).ravel()
    params["faces"] = triangles.ravel()
    params.update()
    mesh.add_attribute("vertex_color", 3, colors.astype(np.float32).ravel())
    return mesh


def render(args):
    """Render the placed skin with the scene, and its isolated coverage."""
    scene = load_scene(args.scene)
    source, job = source_for(scene, args.object, lit=False)
    mi = mr.import_mitsuba()
    if mi is None:
        raise RuntimeError("skin_reference requires Mitsuba")
    mi.set_variant(args.variant or mr.default_variant(mi))
    world, colors, triangles = posed_skin(SkinnedAsset(*load_asset(args.glb)), args.clip, args.time,
                                          args.at, args.heading_deg, args.scale, args.white)
    mesh = skin_shape(mi, world, colors, triangles)
    full = {"type": "scene", "skin": mesh}
    full.update(mr.source_meshes(mi, source, job.settings.double_sided))
    full.update(mr.emitters(mi, [light for light in scene.lights if light["type"] != "ambient"]))
    pose = np.array([*args.eye, *args.fwd])
    width, height = args.size
    path = mr.PathScene(mi, mi.load_dict(full), PIXELS_PER_PASS)
    linear, covered = path.trace(pose, width, height, args.lens, args.near, args.spp)
    args.out.mkdir(parents=True, exist_ok=True)
    np.save(args.out / REFERENCE_LINEAR, linear)
    Image.fromarray(device_picture(linear, covered, scene.tonemap_white,
                                   scene.camera.component.background)).save(args.out / REFERENCE_PICTURE)
    alone = mi.load_dict({"type": "scene", "skin": mesh})
    only = mr.PathScene(mi, alone, PIXELS_PER_PASS)
    _, coverage = only.trace(pose, width, height, args.lens, args.near, MASK_SPP, max_depth=1)
    np.save(args.out / SKIN_COVERAGE, coverage)
    Image.fromarray(np.round(coverage * BYTE_MAX).astype(np.uint8)).save(args.out / SKIN_COVERAGE_PICTURE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True, type=pathlib.Path)
    parser.add_argument("--object")
    parser.add_argument("--glb", required=True, type=pathlib.Path)
    parser.add_argument("--clip")
    parser.add_argument("--time", type=float, required=True)
    parser.add_argument("--at", nargs=3, type=float, required=True)
    parser.add_argument("--heading-deg", type=float, default=0)
    parser.add_argument("--scale", type=float, default=1)
    parser.add_argument("--white", action="store_true")
    parser.add_argument("--eye", nargs=3, type=float, required=True)
    parser.add_argument("--fwd", nargs=3, type=float, required=True)
    parser.add_argument("--size", nargs=2, type=int, required=True)
    parser.add_argument("--lens", type=float, required=True)
    parser.add_argument("--near", type=float, required=True)
    parser.add_argument("--spp", type=int, default=mr.DEFAULT_SPP)
    parser.add_argument("--variant")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    render(parser.parse_args())


if __name__ == "__main__":
    main()
