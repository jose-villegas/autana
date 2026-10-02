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
from r3d.poses import camera_basis, read_poses

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from render_compare import expand_565  # noqa: E402


def camera_rays(width, height, lens, eye, forward, samples):
    """One pinhole ray per subpixel, ordered in pixel-sized groups."""
    right, up, forward = camera_basis(forward)
    x, y = np.meshgrid(np.arange(width), np.arange(height))
    offsets = (np.arange(samples) + 0.5) / samples
    ox, oy = np.meshgrid(offsets, offsets)
    x = (x[..., None] + ox.ravel()).reshape(-1)
    y = (y[..., None] + oy.ravel()).reshape(-1)
    short = min(width, height)
    horizontal = lens * width / short
    vertical = lens * height / short
    direction = forward + right * ((2 * x / width - 1) * horizontal)[:, None]
    direction += up * ((1 - 2 * y / height) * vertical)[:, None]
    direction /= np.linalg.norm(direction, axis=1, keepdims=True)
    return np.repeat(np.asarray(eye, dtype=float)[None, :], len(direction), axis=0), direction


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


def render_linear(source, settings, scene, pose, width, height, lens, samples=4):
    """Return a linear RGB source render, supersampled then box filtered."""
    return trace(source, settings, scene, pose, width, height, lens, samples)[0]


def trace(source, settings, scene, pose, width, height, lens, samples=4):
    """(linear RGB, share of each pixel's subpixels that hit the mesh)."""
    eye, forward = pose[:3], pose[3:]
    origin, direction = camera_rays(width, height, lens, eye, forward, samples)
    locations, rays, faces = source.intersector.intersects_location(origin, direction, multiple_hits=False)
    linear = np.zeros((len(origin), 3), dtype=float)
    covered = np.zeros(len(origin))
    covered[rays] = 1.0
    if len(rays):
        bary = hit_barycentrics(source, faces, locations)
        normal = hit_normals(source, faces, bary)
        material = source.tri_m[faces]
        albedo = hit_albedo(source, faces, bary)
        double = np.isin(material, [source.names.index(name) for name in settings.double_sided])
        radiance = light(locations, normal, double, source.intersector, scene.lights, settings.light.ray_offset,
                         np.random.default_rng(settings.seed), shared_sky_rays=0)
        linear[rays] = albedo * radiance
    return (linear.reshape(height, width, samples * samples, 3).mean(axis=2),
            covered.reshape(height, width, samples * samples).mean(axis=2))


def device_picture(linear, covered, tonemap_white, background):
    """The 8-bit picture the device shows: the tone-mapped mesh over the scene's
    background colour in proportion to what each pixel leaves uncovered, then
    RGB565-quantised."""
    lit = to_srgb8(linear, tonemap_white).astype(float)
    colour = np.array([background >> 16, (background >> 8) & 255, background & 255], dtype=float)
    lit = lit * covered[..., None] + colour * (1.0 - covered[..., None])
    return expand_565(np.round(lit).astype(np.uint8))


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
        linear, covered = trace(source, settings, scene, pose, width, height, lens, args.samples)
        np.save(out / ("%04d.linear.npy" % index), linear)
        picture = device_picture(linear, covered, scene.tonemap_white, scene.camera.component.background)
        Image.fromarray(picture).save(out / ("%04d.png" % index))
    return 0


if __name__ == "__main__":
    sys.exit(main())
