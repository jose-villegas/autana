"""Draw the skeleton and skin figures of a skinned glTF, each animated over one
clip: the joints and their parent links over the mesh, every vertex in its
joints' colours blended by weight, and how far keeping two influences instead
of four moves each vertex.

    python launcher/tools/r3d/skin/skin_sheet.py ASSET.glb IMAGES TABLES [--sheet CLIP[:PHASE]]

Needs Pillow. --sheet names the clip drawn; the first clip when left out.
IMAGES receives skin-skeleton.gif, skin-weights.gif and skin-influences.gif.
TABLES receives skin-influence-counts.md, how many source influences the
vertices have, and skin-weight-example.md, one vertex's weights through
quantize_weights, for generated_blocks.py --tables.
launcher/tools/render/render_doc_images.sh runs it.
"""

import colorsys
import math
import pathlib
import sys

from PIL import ImageDraw

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from r3d import gltf_preview, gltf_skin, skin_asset  # noqa: E402
from r3d.skin_light.skin_light_sheet import TILE, VIEW, grid, heat, parse_clip  # noqa: E402

# Joint colours step round the hue circle by the golden angle, so neighbours differ.
GOLDEN_ANGLE = 0.381966
JOINT_SATURATION = 0.75
JOINT_VALUE = 0.95
JOINT_RADIUS = 3
BONE_WIDTH = 2
BONE = (20, 20, 20)
# Blending heat: white where the heaviest joint holds only this share, as when two joints split evenly.
MIXED_FULL = 0.5
# Vertices that keeping two influences moves past this share of the mesh's bounding radius are ringed.
MOVED_RING = 0.005
RING_RADIUS = 7
RING = (40, 120, 255)
# The zoomed tile frames the vertices with more than two influences, its radius this share of the mesh's.
ZOOM = 0.3
MOVE = (220, 30, 30)


def joint_colour(index):
    r, g, b = colorsys.hsv_to_rgb((index * GOLDEN_ANGLE) % 1.0, JOINT_SATURATION, JOINT_VALUE)
    return int(255 * r), int(255 * g), int(255 * b)


def blended_colours(asset):
    """Each vertex's joint colours weighted by its source weights."""
    out = []
    for joints, weights in zip(asset.joint_indices, asset.weights):
        total = sum(weights) or 1.0
        out.append(tuple(int(round(sum(joint_colour(j)[c] * w for j, w in zip(joints, weights)) / total))
                         for c in range(3)))
    return out


def skin_with(asset, pose, records):
    """Positions skinned with (joints, weights summing to WEIGHT_SUM) per vertex."""
    matrices = asset.joint_matrices(pose)
    return [tuple(sum(gltf_skin.transform_point(matrices[j], p)[a] * w for j, w in zip(joints, weights))
                  / skin_asset.WEIGHT_SUM for a in range(3))
            for p, (joints, weights) in zip(asset.positions, records)]


def records(asset, influences):
    return [skin_asset.quantize_weights(list(joints), list(weights), influences)
            for joints, weights in zip(asset.joint_indices, asset.weights)]


def screen(camera, point):
    x, y, _ = camera.project(point)
    return x / gltf_preview.SUPERSAMPLE, y / gltf_preview.SUPERSAMPLE


def ring(tile, camera, positions, picked):
    draw = ImageDraw.Draw(tile)
    for index in picked:
        x, y = screen(camera, positions[index])
        r = RING_RADIUS
        draw.ellipse((x - r, y - r, x + r, y + r), outline=RING, width=2)
    return tile


def arrows(tile, camera, start, end, picked):
    """A line from each picked vertex's four-influence position to its two-influence one, a dot at the end."""
    draw = ImageDraw.Draw(tile)
    for index in picked:
        a, b = screen(camera, start[index]), screen(camera, end[index])
        draw.line([a, b], fill=MOVE, width=2)
        draw.ellipse((b[0] - 2, b[1] - 2, b[0] + 2, b[1] + 2), fill=MOVE)
    return tile


def zoom_camera(asset, frames, radius):
    """Frames every vertex with more than two influences over the clip's frames."""
    many = [i for i, w in enumerate(asset.weights) if sum(1 for v in w if v > 0) > 2]
    centre, _ = gltf_preview.bounds([frame[i] for frame in frames for i in many])
    return gltf_preview.Camera(centre, radius * ZOOM, *VIEW, TILE[0] * gltf_preview.SUPERSAMPLE,
                               TILE[1] * gltf_preview.SUPERSAMPLE)


def influence_counts(asset, moved):
    """Vertices by nonzero source influences, with the worst move keeping 2 over the clip."""
    rows = ["| Source influences | Vertices | Worst move keeping 2, share of the mesh radius |", "|---:|---:|---:|"]
    counts = [sum(1 for w in weights if w > 0) for weights in asset.weights]
    for count in sorted(set(counts)):
        worst = max(m for m, c in zip(moved, counts) if c == count)
        rows.append(f"| {count} | {counts.count(count)} | {worst:.1%} |")
    return "\n".join(rows)


def weight_example(asset):
    """The first vertex with the most source influences, through quantize_weights at 2 and 4."""
    index = max(range(len(asset.weights)), key=lambda i: sum(1 for w in asset.weights[i] if w > 0))
    pairs = sorted(((j, w) for j, w in zip(asset.joint_indices[index], asset.weights[index]) if w > 0),
                   key=lambda pair: (-pair[1], pair[0]))
    rows = [f"Vertex {index}:", "", "| glTF joint | Source weight | Kept of 4, byte | Kept of 2, byte |",
            "|---:|---:|---:|---:|"]
    kept = {n: {j: w for j, w in zip(*skin_asset.quantize_weights([j for j, _ in pairs], [w for _, w in pairs], n))
                if w} for n in skin_asset.INFLUENCES}
    for joint, weight in pairs:
        rows.append(f"| {joint} | {weight:.4f} | {kept[4].get(joint, '—')} | {kept[2].get(joint, '—')} |")
    rows.append(f"| sum | {sum(w for _, w in pairs):.4f} | {sum(kept[4].values())} | {sum(kept[2].values())} |")
    return "\n".join(rows)


def draw_bones(tile, clip, pose):
    """Each joint as a dot in its colour, a line to its parent joint."""
    asset = clip.asset
    world = asset.world_matrices(pose)
    slot = {node: index for index, node in enumerate(asset.joints)}
    points = {}
    for node in asset.joints:
        points[node] = screen(clip.camera, (world[node][3], world[node][7], world[node][11]))
    draw = ImageDraw.Draw(tile)
    for node in asset.joints:
        parent = asset.parents[node]
        if parent in points:
            draw.line([points[parent], points[node]], fill=BONE, width=BONE_WIDTH)
    for node, (x, y) in points.items():
        r = JOINT_RADIUS
        draw.ellipse((x - r, y - r, x + r, y + r), fill=joint_colour(slot[node]), outline=BONE)
    return tile


def main():
    args, clip = parse_clip(__doc__, "images", "tables")
    asset, name, poses = clip.asset, clip.name, clip.poses
    rest = asset.sample(None, 0)
    weights = blended_colours(asset)
    colours = asset.colors and [tuple(int(round(255 * gltf_preview.linear_to_srgb(c))) for c in rgb[:3])
                                for rgb in asset.colors]
    mixed = [heat((1.0 - max(w) / (sum(w) or 1.0)) / MIXED_FULL) for w in asset.weights]
    two, four = records(asset, 2), records(asset, 4)
    _, radius = gltf_preview.bounds(asset.positions)
    zoom = zoom_camera(asset, clip.positions, radius)

    rest_tile = draw_bones(clip.tile_at(asset.skin(rest)[0], colours, "bind pose"), clip, rest)
    skeleton, weight_frames, influence_frames = [], [], []
    worst = [0.0] * len(asset.positions)
    for k in clip.frames():
        posed = draw_bones(clip.tile(k, colours, f"{name}"), clip, poses[k])
        skeleton.append(grid([rest_tile, posed], 2))
        weight_frames.append(grid([clip.tile(k, weights, "joint colours by weight"),
                                   draw_bones(clip.tile(k, weights, "with joints"), clip, poses[k]),
                                   clip.tile(k, mixed, "joints mixing")], 3,
                                  "joints mixing:  one joint", f"{1 - MIXED_FULL:.0%} / {MIXED_FULL:.0%} or more even"))
        a, b = skin_with(asset, poses[k], two), skin_with(asset, poses[k], four)
        moved = [math.dist(p, q) / radius for p, q in zip(a, b)]
        worst = [max(w, m) for w, m in zip(worst, moved)]
        picked = [i for i, m in enumerate(moved) if m > MOVED_RING]
        influence_frames.append(grid([clip.tile_at(b, colours, "4 influences"),
                                      ring(clip.tile_at(a, colours, "2 influences"), clip.camera, a, picked),
                                      arrows(clip.tile_at(b, colours, "zoomed: 4 to 2", zoom), zoom, b, a, picked)],
                                     3))
    for directory in (args.images, args.tables):
        directory.mkdir(parents=True, exist_ok=True)
    clip.save(args.images / "skin-skeleton.gif", skeleton)
    clip.save(args.images / "skin-weights.gif", weight_frames)
    clip.save(args.images / "skin-influences.gif", influence_frames)
    (args.tables / "skin-influence-counts.md").write_text(influence_counts(asset, worst) + "\n", encoding="utf-8")
    (args.tables / "skin-weight-example.md").write_text(weight_example(asset) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
