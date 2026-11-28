#!/usr/bin/env python3
"""Path trace a scene with posed glTF skins, writing RGB, linear RGB and skin coverage."""
import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from asset.engine_frame import AFFINE_SIGNS
from r3d import mitsuba_reference as mr
from r3d.gltf_skin import SkinnedAsset, load_asset, mat_from_trs, transform_point
from r3d.import_settings import load_scene
from r3d.reference_render import device_picture, source_for

PIXELS_PER_PASS = 1 << 22
MASK_SPP = 16
# Mitsuba depth 1 stops before the first surface interaction.
MASK_DEPTH = 2
MASK_THRESHOLD = 0.5
BYTE_MAX = 255
REFERENCE_PICTURE = "reference.png"
REFERENCE_LINEAR = "reference.linear.npy"
SKIN_COVERAGE = "skin.mask.npy"
SKIN_COVERAGE_PICTURE = "skin.mask.png"
PER_SKIN_COVERAGE = "skins.mask.npy"


def posed_skin(asset, clip, time, at, heading_deg, scale, white=False, world=None):
    """World positions, linear reflectances and triangles for one sampled skin."""
    duration = asset.duration(clip) if clip is not None else 0
    positions, _ = asset.skin(asset.sample(clip, time if world is not None else time % duration if duration > 0 else 0))
    half = np.radians(heading_deg) / 2
    placement = (mat_from_trs(at, (0, np.sin(half), 0, np.cos(half)), (scale,) * 3)
                 if world is None else np.asarray(world, dtype=float).ravel())
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


def pose_frame(frames, index=None, entity=None):
    """Select an exported frame and mirror all matching, fully blended skins."""
    frame = frames[-1] if index is None else next(item for item in frames if item["frame"] == index)
    skins = [skin for skin in frame["skins"] if entity is None or skin["name"] == entity]
    if not skins:
        raise ValueError("pose JSON has no matching skin")
    if any(skin["blend"] != 1 or skin.get("frozen_previous", False) for skin in skins):
        raise ValueError("reference requires a complete blend with no frozen previous pose")
    camera = frame["camera"]
    if camera is None:
        raise ValueError("pose JSON has no active camera")
    return ([{**skin, "world": source_frame(skin["world"])} for skin in skins],
            {**camera, "world": source_frame(camera["world"])})


def source_frame(world):
    """An engine-frame transform from the pose JSON mirrored back into the source frame the scene is traced in."""
    signs = np.diag(AFFINE_SIGNS).astype(float)
    return (signs @ np.asarray(world, dtype=float) @ signs).tolist()


def trace_camera(mi, scene, camera, size, spp, max_depth=mr.DEFAULT_DEPTH):
    """Trace a source-frame camera, which looks down its local -z, including roll; reject off-axis projection."""
    if any(camera.get("shift", (0, 0))):
        raise ValueError("skin reference does not support lens shift")
    world = np.asarray(camera["world"], float)
    eye, forward, up = world[:3, 3], -world[:3, 2], world[:3, 1]
    sensor = mr.sensor(mi, np.r_[eye, forward], *size, camera["lens"], camera["near"])
    parameters = mi.traverse(sensor)
    parameters["to_world"] = mi.ScalarTransform4f.look_at(origin=eye.tolist(), target=(eye + forward).tolist(),
                                                          up=up.tolist())
    parameters.update()
    integrator = mi.load_dict(mr.integrator(max_depth))
    width, height = size
    rgba = np.zeros((height, width, 4))
    batch = max(1, PIXELS_PER_PASS // (width * height))
    for start in range(0, spp, batch):
        count = min(batch, spp - start)
        rgba += np.asarray(mi.render(scene, sensor=sensor, integrator=integrator, spp=count, seed=start)) * (count / spp)
    return rgba[..., :3], np.clip(rgba[..., 3], 0, 1)


def render(args):
    """Render the placed skins with the scene, then trace their isolated coverage."""
    scene = load_scene(args.scene)
    source, job = source_for(scene, args.object, lit=False)
    mi = mr.import_mitsuba()
    if mi is None:
        raise RuntimeError("skin_reference requires Mitsuba")
    mi.set_variant(args.variant or mr.default_variant(mi))
    camera = None
    if args.pose_json is not None:
        skins, camera = pose_frame(json.loads(args.pose_json.read_text()), args.frame, args.entity)
    else:
        skins = [{"clip": args.clip, "time": args.time, "world": None}]
    asset = SkinnedAsset(*load_asset(args.glb))
    meshes = {}
    for index, skin in enumerate(skins, 1):
        positions, colors, triangles = posed_skin(asset, skin["clip"], skin["time"], args.at,
                                                  args.heading_deg, args.scale, args.white, world=skin["world"])
        meshes[f"skin_{index}"] = skin_shape(mi, positions, colors, triangles)
    full = {"type": "scene", **meshes}
    full.update(mr.source_meshes(mi, source, job.settings.double_sided))
    full.update(mr.emitters(mi, [light for light in scene.lights if light["type"] != "ambient"]))
    if camera is None:
        eye, forward = np.asarray(args.eye), np.asarray(args.fwd)
        right = np.cross(forward, (0, 1, 0))
        right /= np.linalg.norm(right)
        matrix = np.eye(4)
        matrix[:3, 0], matrix[:3, 1], matrix[:3, 2], matrix[:3, 3] = right, np.cross(right, forward), -forward, eye
        camera = {"world": matrix.tolist(), "lens": args.lens, "near": args.near}
    linear, covered = trace_camera(mi, mi.load_dict(full), camera, args.size, args.spp)
    args.out.mkdir(parents=True, exist_ok=True)
    np.save(args.out / REFERENCE_LINEAR, linear)
    Image.fromarray(device_picture(linear, covered, scene.tonemap_white,
                                   scene.camera.component.background)).save(args.out / REFERENCE_PICTURE)
    masks = []
    for name, mesh in meshes.items():
        alone = mi.load_dict({"type": "scene", name: mesh})
        _, mask = trace_camera(mi, alone, camera, args.size, MASK_SPP, max_depth=MASK_DEPTH)
        masks.append(mask)
    coverage = masks[0]
    if len(masks) > 1:
        union = mi.load_dict({"type": "scene", **meshes})
        _, coverage = trace_camera(mi, union, camera, args.size, MASK_SPP, max_depth=MASK_DEPTH)
    np.save(args.out / PER_SKIN_COVERAGE, np.stack(masks))
    np.save(args.out / SKIN_COVERAGE, coverage)
    Image.fromarray(np.round(coverage * BYTE_MAX).astype(np.uint8)).save(args.out / SKIN_COVERAGE_PICTURE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scene", required=True, type=pathlib.Path)
    parser.add_argument("--object")
    parser.add_argument("--glb", required=True, type=pathlib.Path)
    parser.add_argument("--clip")
    parser.add_argument("--pose-json", type=pathlib.Path)
    parser.add_argument("--frame", type=int)
    parser.add_argument("--entity")
    parser.add_argument("--time", type=float)
    parser.add_argument("--at", nargs=3, type=float, default=(0, 0, 0))
    parser.add_argument("--heading-deg", type=float, default=0)
    parser.add_argument("--scale", type=float, default=1)
    parser.add_argument("--white", action="store_true")
    parser.add_argument("--eye", nargs=3, type=float)
    parser.add_argument("--fwd", nargs=3, type=float)
    parser.add_argument("--size", nargs=2, type=int, required=True)
    parser.add_argument("--lens", type=float)
    parser.add_argument("--near", type=float)
    parser.add_argument("--spp", type=int, default=mr.DEFAULT_SPP)
    parser.add_argument("--variant")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    if args.spp <= 0 or min(args.size) <= 0:
        parser.error("spp and size must be positive")
    if args.pose_json is None and any(getattr(args, key) is None for key in ("time", "eye", "fwd", "lens", "near")):
        parser.error("use --pose-json or supply --time, --eye, --fwd, --lens and --near")
    render(args)


if __name__ == "__main__":
    main()
