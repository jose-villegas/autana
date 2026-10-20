"""Draw the skeleton and skin figures of a skinned glTF over one clip: the
joints and their parent links over the mesh and every vertex in its joints'
colours blended by weight, animated; and, from the engine's own skinning
(skin_frame.EngineSkin), the clip's frame where keeping two influences instead
of four moves vertices most.

    python launcher/tools/r3d/skin/skin_sheet.py ASSET.glb IMAGES TABLES [--sheet CLIP[:PHASE]]

Needs Pillow, a C compiler and the mesh baker's dependencies. --sheet names the clip drawn; the first clip when left out.
IMAGES receives skin-skeleton.gif, skin-weights.gif and skin-influences.png.
TABLES receives skin-influence-counts.md, how many source influences the
vertices have, and skin-weight-example.md, one vertex's weights through
quantize_weights, for generated_blocks.py --tables.
launcher/tools/render/render_doc_images.sh runs it.
"""

import colorsys
import math
import pathlib
import sys
import tempfile

from PIL import ImageDraw

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from r3d import gltf_preview, skin_asset  # noqa: E402
from r3d.skin.skin_frame import DEFAULT_POSITION_SCALE, INFLUENCES, EngineSkin  # noqa: E402
from r3d.skin_light.skin_light_sheet import TILE, VIEW, grid, heat, label, parse_clip  # noqa: E402

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
# The influence figure looks up at the mesh from this pitch, degrees: the vertices that move most are underneath.
UNDER_PITCH = -30.0
# Its whole-mesh tiles frame this share of the clip's bounding radius.
WHOLE = 0.9
RING_RADIUS = 7
RING = (40, 120, 255)
# The zoomed tiles frame the vertex that moves most, their radius this share of the mesh's.
ZOOM = 0.25
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


def below(centre, radius):
    return gltf_preview.Camera(centre, radius, VIEW[0], UNDER_PITCH, TILE[0] * gltf_preview.SUPERSAMPLE,
                               TILE[1] * gltf_preview.SUPERSAMPLE)


def influence_figure(clip, engine, radius):
    """The engine's skin at two and four influences on the clip's frame where they differ most, seen from below,
    whole and zoomed on the vertex that moves most; and each source vertex's worst move over the clip."""
    name, count = clip.name, len(clip.poses)
    times = {k: int(round(1000 * clip.asset.duration(name) * k / count)) for k in clip.frames()}
    runs = {k: {n: engine.positions(name, ms, n) for n in INFLUENCES} for k, ms in times.items()}
    moved = {k: [math.dist(a, b) / radius for a, b in zip(run[2], run[4])] for k, run in runs.items()}
    frame = max(moved, key=lambda k: max(moved[k]))
    two, four = runs[frame][2], runs[frame][4]
    target = max(range(len(four)), key=moved[frame].__getitem__)
    picked = [v for v, m in enumerate(moved[frame]) if m > MOVED_RING]
    centre, extent = gltf_preview.bounds([p for frame_positions in clip.positions for p in frame_positions])
    whole, zoom = below(centre, extent * WHOLE), below(four[target], radius * ZOOM)

    def tile(camera, positions, text):
        image = gltf_preview.render(engine.rendered, camera, positions, TILE, engine.lit, ground=False)
        return label(ring(image, camera, positions, picked), text)

    when = f"{name} {times[frame]} ms"
    tiles = [tile(whole, four, f"engine, 4 influences, {when}"), tile(whole, two, "engine, 2 influences"),
             tile(zoom, four, "zoomed, 4 influences"),
             arrows(tile(zoom, two, "zoomed, 2: red from 4 to 2"), zoom, four, two, picked)]
    worst = [max(m[engine.match(i)] for m in moved.values()) for i in range(len(clip.asset.positions))]
    return grid(tiles, 2), worst


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
    _, radius = gltf_preview.bounds(asset.positions)

    rest_tile = draw_bones(clip.tile_at(asset.skin(rest)[0], colours, "bind pose"), clip, rest)
    skeleton, weight_frames = [], []
    for k in clip.frames():
        posed = draw_bones(clip.tile(k, colours, f"{name}"), clip, poses[k])
        skeleton.append(grid([rest_tile, posed], 2))
        weight_frames.append(grid([clip.tile(k, weights, "joint colours by weight"),
                                   draw_bones(clip.tile(k, weights, "with joints"), clip, poses[k]),
                                   clip.tile(k, mixed, "joints mixing")], 3,
                                  "joints mixing:  one joint", "split evenly"))
    with tempfile.TemporaryDirectory(prefix="skin-sheet-") as scratch:
        engine = EngineSkin(args.asset, DEFAULT_POSITION_SCALE, pathlib.Path(scratch))
        influences, worst = influence_figure(clip, engine, radius)
    for directory in (args.images, args.tables):
        directory.mkdir(parents=True, exist_ok=True)
    clip.save(args.images / "skin-skeleton.gif", skeleton)
    clip.save(args.images / "skin-weights.gif", weight_frames)
    influences.save(args.images / "skin-influences.png", optimize=True)
    (args.tables / "skin-influence-counts.md").write_text(influence_counts(asset, worst) + "\n", encoding="utf-8")
    (args.tables / "skin-weight-example.md").write_text(weight_example(asset) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
