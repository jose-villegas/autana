"""Render firmware-skinned pack positions beside gltf_skin, including selected vertex crops.

Takes a glTF model, clip:millisecond poses and source vertex indices; no model-specific defaults.
Uses the existing mesh/skin/track writers, preview rasterizer and comparison sheets.
"""
import argparse
import json
import math
import pathlib
import subprocess
import sys
import tempfile
from types import SimpleNamespace

from PIL import Image, ImageDraw

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from anim import skeleton_asset, track_host, tracks_asset
from asset.asset_pack import build_pack
from build import packages
from gltf import gltf_read
from r3d import gltf_preview, gltf_skin, lit_mesh, skin_asset
from scene import scene_host
sys.path.insert(0, str(TOOLS / "render"))
import render_compare

DEFAULT_POSITION_SCALE = 16384
DEFAULT_SIZE = 320
CROP_RADIUS = 12
CROP_ZOOM = 4
CROP_CAPTION_WIDTH = 240
INFLUENCES = (2, 4)
MESH_ID = "mesh"
SKIN_ID = "skin"
CLIP_ID = "clip"
SOURCES = [pathlib.Path(__file__).with_name("skin_host.c"), *scene_host.SOURCES]


def program(directory):
    target = directory / ("skin_host.exe" if sys.platform == "win32" else "skin_host")
    includes = [arg for root in packages.include_roots() for arg in ("-I", str(root))]
    subprocess.run([track_host.compiler(), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-Werror=double-promotion", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections", *includes, *map(str, SOURCES),
                    "-lm", "-o", str(target)], check=True)
    return target


def run_pose(target, directory, entries, skeleton, clip, ms):
    pack = directory / "pose.pack"
    pack.write_bytes(build_pack(entries))
    result = subprocess.run([str(target), str(pack), scene_host.SCENE_ID, scene_host.ENTITY_NAME, skeleton, SKIN_ID,
                             MESH_ID, CLIP_ID, str(ms)],
                            text=True, capture_output=True, check=True)
    return [tuple(int(v) for v in line.split()) for line in result.stdout.splitlines()]


class EngineSkin:
    """A glTF model packed by the entry writers and posed by the scene's animation and skin systems in skin_host.c,
    at each of INFLUENCES. `mesh` is its LMSH bake, `rendered` its triangles for gltf_preview.render."""

    def __init__(self, model, scale, directory):
        self.document, self.binary = gltf_read.load_glb(model)
        self.asset = gltf_skin.SkinnedAsset(self.document, self.binary)
        self.mesh_node = next(i for i, n in enumerate(self.document["nodes"]) if "skin" in n and "mesh" in n)
        self.ordered, _, self.skeleton_name = skeleton_asset.rig(self.document, self.mesh_node)
        self.colors = [tuple(round(gltf_preview.linear_to_srgb(c) * 255) for c in rgb[:3])
                       for rgb in (self.asset.colors or [(0.6,) * 3] * len(self.asset.positions))]
        self.scale = scale
        self.mesh = lit_mesh.bake_lit_mesh(self.asset.positions, self.colors, self.asset.triangles,
                                           [True] * len(self.asset.triangles), position_scale=scale)
        self.mesh_bytes = lit_mesh.mesh_blob(self.mesh)
        self.scene_bytes = scene_host.one_entity_scene(MESH_ID)
        self.variants = {n: skin_asset.bake(self.document, self.binary, self.mesh_node, self.mesh_bytes, n)
                         for n in INFLUENCES}
        self.rendered = SimpleNamespace(triangles=self.mesh.tris.tolist())
        self.lit = [tuple(rgb) for rgb in self.mesh.rgb.tolist()]
        self.directory = directory
        self.target = program(directory)
        self._clips = {}

    def match(self, source_vertex):
        """The LMSH vertex a source vertex quantizes to."""
        if not hasattr(self, "_by_position"):
            self._by_position = {tuple(p): i for i, p in enumerate(self.mesh.pos.tolist())}
        return self._by_position[skin_asset.quantize_position(self.asset.positions[source_vertex], self.scale)]

    def positions(self, clip, ms, influences):
        """Model-space positions of every LMSH vertex, as the engine skins them at `ms` into `clip`."""
        if clip not in self._clips:
            tracks, duration, root = tracks_asset.clip_tracks(self.document, self.binary, self.asset.animations[clip])
            self._clips[clip] = tracks_asset.encode(tracks, duration, root)
        _, skeleton, skin = self.variants[influences]
        entries = [(MESH_ID, b"LMSH", self.mesh_bytes), (self.skeleton_name, b"SKEL", skeleton),
                   (SKIN_ID, b"SKIN", skin), (CLIP_ID, b"TRCK", self._clips[clip]), (scene_host.SCENE_ID, b"SCNE", self.scene_bytes)]
        ticks = run_pose(self.target, self.directory, entries, self.skeleton_name, clip, ms)
        world = self.asset.world_matrices(self.asset.sample(clip, ms / 1000))[self.mesh_node]
        return [gltf_skin.transform_point(world, tuple(v / self.scale for v in p)) for p in ticks]


def quantized_reference(asset, pose, ordered, records, positions, scale):
    matrices = asset.joint_matrices(pose)
    palette = [matrices[asset.joints.index(node)] for node in ordered]
    result = []
    for p, (joints, weights, _) in zip(positions, records):
        points = [gltf_skin.transform_point(palette[j], tuple(float(v) / scale for v in p)) for j in joints]
        result.append(tuple(sum(point[a] * w / skin_asset.WEIGHT_SUM for point, w in zip(points, weights))
                            for a in range(3)))
    return result


def crops(rows, clip, ms, camera, reference, indices, images, variant_positions, matches):
    for index in indices:
        x, y, _ = camera.project(reference[index])
        x, y = round(x / gltf_preview.SUPERSAMPLE), round(y / gltf_preview.SUPERSAMPLE)
        box = (x - CROP_RADIUS, y - CROP_RADIUS, x + CROP_RADIUS, y + CROP_RADIUS)
        tiles = [image.crop(box).resize((2 * CROP_RADIUS * CROP_ZOOM,) * 2, Image.Resampling.NEAREST)
                 for image in images]
        for tile, positions in zip(tiles, variant_positions):
            px, py, _ = camera.project(positions[matches[index]])
            px = (px / gltf_preview.SUPERSAMPLE - box[0]) * CROP_ZOOM
            py = (py / gltf_preview.SUPERSAMPLE - box[1]) * CROP_ZOOM
            ImageDraw.Draw(tile).ellipse((px - CROP_ZOOM, py - CROP_ZOOM, px + CROP_ZOOM, py + CROP_ZOOM), outline="red", width=2)
        label = f"vertex {index}: {clip} {ms} ms"
        rows.append((label, *[render_compare.captioned(tile, label, width=CROP_CAPTION_WIDTH) for tile in tiles]))


def compare(args, engine):
    asset, mesh, colors = engine.asset, engine.mesh, engine.colors
    matches = {i: engine.match(i) for i in args.vertices}
    rest_positions, _ = asset.skin(asset.sample(None, 0))
    source = SimpleNamespace(positions=rest_positions, triangles=asset.triangles)
    size = (args.size,) * 2
    camera = gltf_preview.make_camera(source, args.view, size)
    rendered = engine.rendered
    reference_rows, influence_rows, crop_rows, stats = [], [], [], []
    args.out.mkdir(parents=True, exist_ok=True)
    for clip, ms in args.poses:
        reference_pose = asset.sample(clip, ms / 1000)
        reference, _ = asset.skin(reference_pose)
        reference_image = gltf_preview.render(source, camera, reference, size, lit=colors)
        images, variant_positions = [], []
        for influences in INFLUENCES:
            positions = engine.positions(clip, ms, influences)
            image = gltf_preview.render(rendered, camera, positions, size, lit=engine.lit)
            images.append(image)
            variant_positions.append(positions)
            _, records, _, _ = skin_asset.decode(engine.variants[influences][2])
            packed_reference = quantized_reference(asset, reference_pose, engine.ordered, records, mesh.pos, args.scale)
            error = max(math.dist(a, b) for a, b in zip(positions, packed_reference))
            stats.append(dict(clip=clip, ms=ms, influences=influences, max_position_error=error,
                              position_tick=1 / args.scale, vertices=len(positions)))
            if error > 2 / args.scale:
                raise ValueError(f"C kernel differs from packed-weight reference by {error}")
            stem = f"{clip}-{ms}-{influences}"
            image.save(args.out / (stem + ".png"))
            reference_rows.append((stem, image, reference_image))
        label = f"{clip} {ms} ms"
        influence_rows.append((label, *[render_compare.captioned(im, label) for im in images]))
        stats.append(dict(clip=clip, ms=ms, selected_vertex_displacement={
            str(i): math.dist(variant_positions[0][matches[i]], variant_positions[1][matches[i]]) for i in args.vertices}))
        crops(crop_rows, clip, ms, camera, reference, args.vertices, images, variant_positions, matches)
    render_compare.reference_sheet(reference_rows, "C skinning (bind colours)", tile=1).save(args.out / "reference-sheet.png")
    render_compare.sheet(influence_rows, None, "2 influences", "4 influences").save(args.out / "influence-sheet.png")
    if crop_rows:
        render_compare.sheet(crop_rows, None, "2 influences", "4 influences").save(args.out / "vertex-crops.png")
    (args.out / "measurements.json").write_text(json.dumps(stats, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(stats))


def pose(value):
    clip, ms = value.rsplit(":", 1)
    return clip, int(ms)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("model", type=pathlib.Path)
    parser.add_argument("--poses", type=pose, nargs="+", required=True, metavar="CLIP:MS")
    parser.add_argument("--vertices", type=lambda s: [int(i) for i in s.split(",")], default=[])
    parser.add_argument("--scale", type=int, default=DEFAULT_POSITION_SCALE)
    parser.add_argument("--size", type=int, default=DEFAULT_SIZE)
    parser.add_argument("--view", choices=gltf_preview.VIEWS, default="three-quarter")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="skin-frame-") as scratch:
        directory = pathlib.Path(scratch)
        compare(args, EngineSkin(args.model, args.scale, directory))


if __name__ == "__main__":
    main()
