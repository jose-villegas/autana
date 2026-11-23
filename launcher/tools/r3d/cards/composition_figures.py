"""How carded foliage composes, at one pose of a scene's camera path: the four figures of Appearance-Fit.md's
"Alpha and cards", traced from the import source with the scene's bake lighting.

    python launcher/tools/r3d/cards/composition_figures.py SCENE --object NAME --time-ms MS --out DIR

cards-<object>-source.png: the source with its foliage drawn, lit as it is baked.
cards-<object>-light-only.png: the foliage hidden from the camera; its shadows and bounced light stay.
cards-<object>-cards.png: the accepted cards alone, over the background, in the colours the import bakes into them.
cards-<object>-composite.png: the cards over the light-only render wherever they are nearer: the picture the fitted rest and the cards
approximate.
"""
import argparse
import pathlib
import sys

import numpy as np
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[1]))
from docs.figures import save_png  # noqa: E402
from r3d.cards import emit as card_emit  # noqa: E402
from r3d.cards.pages import PAGE_SIDE  # noqa: E402
from r3d.geometry import hit_barycentrics  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from r3d.light import AlphaTest, light  # noqa: E402
from r3d.mesh_source import with_triangles  # noqa: E402
from r3d.obj import Texture  # noqa: E402
from r3d.poses import sample_camera_path  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.reference_render import camera_hidden, device_picture, primary_hits, source_for, trace  # noqa: E402

STEPS = ("source", "light-only", "cards", "composite")
# The panel's size, so a figure shows what the board's pixels can.
PANEL_SIZE = (368, 448)
# Rays per pixel along each side, box filtered.
SAMPLES = 2


def path_pose(scene, time_ms, width, height):
    """The camera path's pose `time_ms` into its clip."""
    camera = scene.camera.component
    if camera.path is None:
        raise ValueError("the scene camera has no path")
    if time_ms <= 0:
        raise ValueError("--time-ms must be positive")
    *_, poses = sample_camera_path(camera.path.animation, camera.path.node, time_ms, width, height,
                                   camera.half_fov_short_tan, camera.near_z)
    if len(poses) < 2:
        raise ValueError(f"the camera clip is shorter than {time_ms} ms")
    return poses[1], camera.half_fov_short_tan


def card_source(source, accepted, job, scene):
    """(every accepted card as one source, its quads with MASK1 pages as alpha textures; each vertex's linear
    colour)."""
    p, uv, tri_v, tri_m, textures, albedo, normal = [], [], [], [], [], [], []
    for _, shape, _ in accepted:
        base = sum(len(points) for points in p)
        for page in shape['pages']:
            rgba = np.ones((PAGE_SIDE, PAGE_SIDE, 4))
            rgba[..., 3] = page.levels[0]
            textures.append(Texture.from_levels([rgba]))
        page_uv = shape['vertex_uvs'] / (card_emit.UV_SCALE * PAGE_SIDE)
        p.append(shape['positions'])
        uv.append(np.column_stack([page_uv[:, 0], 1 - page_uv[:, 1]]))
        tri_v.append(shape['tris'] + base)
        first_page = len(textures) - len(shape['pages'])
        tri_m.append(np.repeat(shape['page_of_card'], len(card_emit.QUAD_TRIANGLES)) + first_page)
        albedo.append(shape['albedo'])
        normal.append(shape['normals'])
    tri_v = np.concatenate(tri_v)
    cards = with_triangles(source, tri_v, tri_v, np.concatenate(tri_m), p=np.concatenate(p), uv=np.concatenate(uv),
                           textures=textures)
    return cards, lit_vertices(cards.p, np.concatenate(normal), np.concatenate(albedo), source, job, scene)


def lit_vertices(points, normals, albedo, source, job, scene):
    """Linear colour at card vertices: albedo under the bake lighting, both sides lit, as the import bakes them."""
    radiance = light(points, normals, np.ones(len(points), bool), source.intersector, scene.lights,
                     job.bake.ray_offset, source.bounce, ao=job.bake.ao,
                     bounce_intensity=scene.indirect.intensity if source.bounce is not None else 1.0)
    return albedo * radiance


def card_layer(cards, vertex_linear, keep_alpha, origin, direction):
    """(hit, distance, linear colour) of each ray against the cards alone, cut by their masks."""
    query = RayQuery(cards.p, cards.tri_v, hit_filter=AlphaTest(cards, keep_alpha))
    hit, distance, tri = query.first_hits(origin, direction)
    linear = np.zeros((len(origin), 3))
    if hit.any():
        rays = np.flatnonzero(hit)
        where = origin[rays] + distance[rays, None] * direction[rays]
        bary = hit_barycentrics(cards.p, cards.tri_v, tri[rays], where)
        linear[rays] = (vertex_linear[cards.tri_v[tri[rays]]] * bary[:, :, None]).sum(axis=1)
    return hit, distance, linear


def picture(linear, covered, width, height, scene):
    """The device picture of linear colour and coverage traced SAMPLES times finer on each axis, box filtered."""
    linear = linear.reshape(height, SAMPLES, width, SAMPLES, 3).mean(axis=(1, 3))
    covered = covered.reshape(height, SAMPLES, width, SAMPLES).mean(axis=(1, 3))
    return Image.fromarray(device_picture(linear, covered, scene.tonemap_white, scene.camera.component.background))


def render(scene_path, name, time_ms):
    """The four step pictures, by STEPS name."""
    scene = load_scene(scene_path)
    width, height = PANEL_SIZE
    pose, lens = path_pose(scene, time_ms, width, height)
    source, job = source_for(scene, name)
    accepted, reports = card_emit.carded(source, job.settings, scene)
    if not accepted:
        raise ValueError(f"{name}: no material is accepted as cards: " + ", ".join(
            f"{material} covers {report['covered']:.2f}" for material, report in reports.items()))
    # One ray per subpixel, filtered by picture(), so the composite can pick cards or reference per ray.
    rays = (width * SAMPLES, height * SAMPLES)
    shown = trace(source, job, scene, pose, *rays, lens, 1)
    source.camera_hidden = camera_hidden(source, job.settings, hidden_materials=[
        source.names[material] for material, _, _ in accepted])
    hidden = trace(source, job, scene, pose, *rays, lens, 1)
    origin, direction, locations, hit_rays, *_ = primary_hits(source, job, pose, *rays, lens, 1)
    behind = np.full(len(origin), np.inf)
    behind[hit_rays] = np.einsum("ij,ij->i", locations - origin[hit_rays], direction[hit_rays]) / np.einsum(
        "ij,ij->i", direction[hit_rays], direction[hit_rays])
    cards, vertex_linear = card_source(source, accepted, job, scene)
    card_hit, card_distance, card_linear = card_layer(cards, vertex_linear, job.settings.alpha_keep, origin, direction)
    front = card_hit & (card_distance < behind)
    hidden_linear, hidden_covered = hidden[0].reshape(-1, 3), hidden[1].reshape(-1)
    composite = np.where(front[:, None], card_linear, hidden_linear)
    return dict(zip(STEPS, (
        picture(shown[0].reshape(-1, 3), shown[1].reshape(-1), width, height, scene),
        picture(hidden_linear, hidden_covered, width, height, scene),
        picture(card_linear, card_hit.astype(float), width, height, scene),
        picture(composite, np.maximum(hidden_covered, front), width, height, scene))))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene")
    parser.add_argument("--object", required=True, help="the scene's mesh renderer whose import is carded")
    parser.add_argument("--time-ms", type=int, required=True, help="the pose: this far into the camera path's clip")
    parser.add_argument("--out", required=True, help="folder the four PNGs go into")
    args = parser.parse_args(argv)
    try:
        steps = render(args.scene, args.object, args.time_ms)
    except ValueError as error:
        parser.error(str(error))
    for step, image in steps.items():
        save_png(image, pathlib.Path(args.out) / f"cards-{args.object}-{step}.png")
    return 0


if __name__ == "__main__":
    sys.exit(main())
