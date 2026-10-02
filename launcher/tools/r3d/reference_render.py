#!/usr/bin/env python3
"""Render an undecimated import source with the scene's bake lighting.

    python launcher/tools/r3d/reference_render.py SCENE.scene.toml --poses poses.txt --out DIR

The poses file is the ``size``, ``lens`` and ``pose`` format emitted by
tools/anim/sample_tracks.sh.  Each pose writes a floating-point .npy image and
an RGB565-expanded PNG.  The renderer deliberately has no scene knowledge:
the scene supplies the source import, lights, camera lens and pose path.
"""

import argparse
import pathlib
import sys

import numpy as np
import trimesh
from PIL import Image
from trimesh.ray.ray_pyembree import RayMeshIntersector

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d.geometry import corner_normals
from r3d.import_settings import load_scene
from r3d.light import albedo_from_uv, drop_masked, light, to_srgb8
from r3d.mesh_import import load_source
from r3d.poses import camera_rays, read_poses

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from render_compare import expand_565  # noqa: E402


def hit_barycentrics(source, hits, locations):
    """Barycentric weights of ray hits on their source triangles."""
    faces = source.tri_v[hits]
    a, b, c = source.p[faces[:, 0]], source.p[faces[:, 1]], source.p[faces[:, 2]]
    total = np.abs(np.cross(b - a, c - a)).sum(axis=1)
    wa = np.abs(np.cross(b - locations, c - locations)).sum(axis=1) / total
    wb = np.abs(np.cross(c - locations, a - locations)).sum(axis=1) / total
    return np.stack([wa, wb, 1.0 - wa - wb], axis=1)


def hit_normals(source, hits, bary):
    """Smooth the import's crease-limited source normals at ray hits."""
    corners = source.corner_normals[hits]
    normal = (corners * bary[:, :, None]).sum(axis=1)
    return normal / np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)


def hit_albedo(source, hits, bary):
    """Sample each hit's source texture at the source triangle's UV point."""
    material = source.tri_m[hits]
    out = np.zeros((len(hits), 3), dtype=float)
    for index in np.unique(material):
        chosen = material == index
        kd = source.materials.get(source.names[index], {}).get("Kd", (1.0, 1.0, 1.0))
        uv = (source.uv[source.tri_t[hits[chosen]]] * bary[chosen, :, None]).sum(axis=1)
        out[chosen] = albedo_from_uv(source.textures[index], kd, uv, np.zeros(len(uv)))
    return out


def render_linear(source, settings, scene, pose, width, height, lens, samples=4, normals=False):
    """Return a linear RGB source render, supersampled then box filtered;
    with `normals`, also the world-space shading normal per pixel, turned
    toward the eye, averaged over the pixel's rays and zero where they all
    miss."""
    eye, forward = pose[:3], pose[3:]
    origin, direction = camera_rays(width, height, lens, eye, forward, samples)
    locations, rays, faces = source.intersector.intersects_location(origin, direction, multiple_hits=False)
    linear = np.zeros((len(origin), 3), dtype=float)
    shading = np.zeros((len(origin), 3), dtype=float)
    if len(rays):
        bary = hit_barycentrics(source, faces, locations)
        normal = hit_normals(source, faces, bary)
        material = source.tri_m[faces]
        albedo = hit_albedo(source, faces, bary)
        double = np.isin(material, [source.names.index(name) for name in settings.double_sided])
        radiance = light(locations, normal, double, source.intersector, scene.lights, settings.light.ray_offset,
                         np.random.default_rng(settings.seed), shared_sky_rays=0)
        linear[rays] = albedo * radiance
        shading[rays] = normal * np.where((normal * direction[rays]).sum(axis=1) > 0, -1.0, 1.0)[:, None]
    picture = linear.reshape(height, width, samples * samples, 3).mean(axis=2)
    if not normals:
        return picture
    shading = shading.reshape(height, width, samples * samples, 3).sum(axis=2)
    return picture, shading / np.maximum(np.linalg.norm(shading, axis=2, keepdims=True), 1e-12)


def parse_rgb(text):
    """An RGB clear colour from a six-digit hex string."""
    if len(text) != 6:
        raise ValueError("clear colour must be RRGGBB")
    return np.array([int(text[index : index + 2], 16) for index in (0, 2, 4)], dtype=np.uint8)


def source_for(scene, import_path=None):
    """Load one placed source at full detail, applying alpha masking."""
    renderer = scene.renderers[0]
    if import_path is not None:
        wanted = pathlib.Path(import_path).resolve()
        matches = [item for item in scene.renderers if item.settings.path == wanted]
        if not matches:
            raise ValueError("the import is not placed by the scene")
        renderer = matches[0]
    settings = renderer.settings
    source = load_source(settings)
    if settings.alpha_keep is not None:
        source.tri_v, source.tri_t, source.tri_m = drop_masked(source.p, source.uv, source.tri_v, source.tri_t,
                                                                 source.tri_m, source.textures, settings.alpha_keep)
    source.corner_normals = corner_normals(source.p, source.tri_v)
    source.intersector = RayMeshIntersector(trimesh.Trimesh(source.p, source.tri_v, process=False))
    return source, settings


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene")
    parser.add_argument("--import", dest="import_path", help="placed .import.toml source, when the scene has several")
    parser.add_argument("--poses", required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--samples", type=int, default=4)
    parser.add_argument("--skip", type=int, default=0, help="ignore this many leading poses")
    parser.add_argument("--clear", type=parse_rgb, help="RGB clear colour for rays that miss")
    parser.add_argument("--normals", action="store_true", help="also write each pose's shading normals as NNN.normal.npy")
    args = parser.parse_args(argv)
    if args.samples < 1 or args.skip < 0:
        parser.error("--samples must be positive and --skip cannot be negative")
    scene = load_scene(args.scene)
    width, height, lens, _near, poses = read_poses(args.poses)
    poses = poses[args.skip :]
    if not poses:
        parser.error("--skip removes every pose")
    if scene.camera is None or scene.tonemap_white is None:
        parser.error("scene needs a camera and tone map")
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    try:
        source, settings = source_for(scene, args.import_path)
    except ValueError as error:
        parser.error(str(error))
    for index, pose in enumerate(poses):
        linear = render_linear(source, settings, scene, pose, width, height, lens, args.samples, args.normals)
        if args.normals:
            linear, normal = linear
            np.save(out / ("%03d.normal.npy" % index), normal.astype(np.float32))
        if args.clear is not None:
            linear[~linear.any(axis=2)] = (args.clear / 255.0) ** 2.2
        np.save(out / ("%03d.linear.npy" % index), linear)
        Image.fromarray(expand_565(to_srgb8(linear, scene.tonemap_white).astype(np.uint8))).save(out / ("%03d.png" % index))
    return 0


if __name__ == "__main__":
    sys.exit(main())
