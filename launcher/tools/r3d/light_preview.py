#!/usr/bin/env python3
"""Try a scene's sun at other rotations before baking: a path-traced still per rotation and camera pose, and the
share of chosen materials in direct sun. --save writes the one rotation given into the scene file.

    python launcher/tools/r3d/light_preview.py SCENE.scene.toml --rotation PITCH YAW [--rotation PITCH YAW ...]
        [--camera NAME] [--at SECONDS ...] [--share MATERIAL ...] [--object NAME] [--light NAME]
        [--indirect K ...] [--size WxH] [--spp N] [--out PNG] [--no-show] [--save]

A rotation is the light object's [pitch, yaw] in degrees, as the scene file writes it (roll is kept): pitch is the
angle from straight up, yaw turns it about the vertical. --camera names a camera object (default: the first) whose
path is sampled at each --at time (default: 0). --share takes material names or fnmatch patterns ("fabric_*") of
the --object renderer (default: the first) and prints, per rotation, the share of their area in direct sun and,
per pose, of their visible pixels. The sheet has one row per rotation, the scene's rotation first. Run from a
terminal, it opens the sheet in the image viewer when written; --no-show (or output to a pipe) only prints its path.

The stills are r3d.mitsuba_reference's, at the scene's exposure and tone map, traced to the bake's bounce count
with its albedo boost. --indirect K scales their bounced light as the bake's [indirect] intensity does: each tile
is the direct light plus K times what the bounces add (a direct-only trace subtracted from the full one); one row
per rotation and K, K the scene's intensity when not given (0 when the renderer bakes no bounced light). Its path
tracer has no ambient term, so a scene's [ambient] is left out of the stills.
"""

import argparse
import fnmatch
import math
import os
import pathlib
import re
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import tracks_asset  # noqa: E402
from r3d import mitsuba_reference  # noqa: E402
from r3d.import_settings import load_scene, rotate  # noqa: E402
from r3d.poses import camera_rays  # noqa: E402
from r3d.ray_query import RayQuery  # noqa: E402
from r3d.reference_render import device_picture, source_for  # noqa: E402

# Area samples per --share pattern: about 0.5% noise on its share in sun.
AREA_SAMPLES = 40000
# Height of a caption line under a tile, and width of the label column, in pixels.
CAPTION = 16
LABEL_WIDTH = 260
# Mitsuba's path depth of direct light alone: the camera ray and the light's.
DIRECT_DEPTH = 2


def toward(rotation):
    """The unit direction toward the light at a [pitch, yaw, roll] rotation."""
    direction = np.array(rotate(rotation, [0.0, 1.0, 0.0]))
    return direction / np.linalg.norm(direction)


def short(value):
    """A number in three decimals at most, with no trailing zeros and no negative zero."""
    text = ("%.3f" % value).rstrip("0").rstrip(".")
    return "0" if text == "-0" else text


def describe(rotation):
    d = toward(rotation)
    return "rotation [%s] toward (%s, 1, %s) elevation %.1f" % (", ".join(short(v) for v in rotation),
        short(d[0] / d[1]), short(d[2] / d[1]), math.degrees(math.asin(d[1])))


def save_rotation(text, name, rotation):
    """The scene file text with object `name`'s rotation line set to `rotation`, the comment after it naming the
    direction toward it; the rest of the file is kept byte for byte."""
    blocks = list(re.finditer(r"^\[\[objects\]\]\s*$", text, re.M)) + [None]
    for start, end in zip(blocks, blocks[1:]):
        body = text[start.end():end.start() if end else len(text)]
        if not re.search(r'^name\s*=\s*"%s"\s*$' % re.escape(name), body, re.M):
            continue
        d = toward(rotation)
        line = "rotation = [%s]   # toward (%s, 1.0, %s)" % (", ".join("%r" % float(v) for v in rotation),
                                                             short(d[0] / d[1]), short(d[2] / d[1]))
        found = re.subn(r"^rotation\s*=[^\r\n]*", lambda _: line, body, count=1, flags=re.M)
        if not found[1]:
            found = re.subn(r"^(name\s*=[^\r\n]*)(\r?\n)", lambda m: m.group(1) + m.group(2) + line + m.group(2), body,
                            count=1, flags=re.M)
        offset = start.end()
        return text[:offset] + found[0] + text[offset + len(body):]
    raise ValueError(f"the scene file has no object named {name!r}")


def path_poses(animation, node, times):
    """[eye, forward] of camera node `node` at each time in seconds along the clip a .anim.toml names, by the Python
    sampler (no compiler needed): its translation, and its rotation turning -Z, as track_host --poses writes them."""
    tracks, duration_ms = tracks_asset.decode(tracks_asset.bake(animation))
    found = {track["name"]: track for track in tracks}
    if f"{node}/translation" not in found or f"{node}/rotation" not in found:
        raise ValueError(f"{animation}: no translation and rotation tracks for node {node!r}")
    poses = []
    for seconds in times:
        seconds = seconds % (duration_ms / 1000) if duration_ms else 0.0
        eye = np.array(tracks_asset.sample(found[f"{node}/translation"], seconds))
        x, y, z, w = tracks_asset.sample(found[f"{node}/rotation"], seconds)
        # The quaternion turning (0, 0, -1).
        forward = -np.array([2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)])
        poses.append(np.concatenate([eye, forward]))
    return poses


def area_samples(source, triangles, count, rng):
    """`count` points spread by area over `triangles`, with their face normals."""
    a, b, c = (source.p[source.tri_v[triangles, k]] for k in range(3))
    cross = np.cross(b - a, c - a)
    area = np.linalg.norm(cross, axis=1)
    pick = rng.choice(len(triangles), count, p=area / area.sum())
    s, t = np.sqrt(rng.random(count)), rng.random(count)
    points = (1 - s)[:, None] * a[pick] + (s * (1 - t))[:, None] * b[pick] + (s * t)[:, None] * c[pick]
    return points, cross[pick] / area[pick, None]


def in_sun(query, points, normals, sun, offset):
    """Whether each point is lit by the sun: the side of a surface facing it, with nothing in the way."""
    normals = normals * np.where(normals @ sun < 0, -1.0, 1.0)[:, None]
    origins = points + normals * offset
    return ~query.blocked(origins, np.ascontiguousarray(np.broadcast_to(sun, origins.shape)))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene")
    parser.add_argument("--rotation", nargs=2, type=float, action="append", required=True, metavar=("PITCH", "YAW"))
    parser.add_argument("--light", help="the light object (default: the scene's only one)")
    parser.add_argument("--object", help="the mesh renderer to render (default: the first)")
    parser.add_argument("--camera", help="the camera object whose path gives the poses (default: the first)")
    parser.add_argument("--at", nargs="+", type=float, default=[0.0], metavar="SECONDS")
    parser.add_argument("--share", nargs="+", default=[], metavar="MATERIAL")
    parser.add_argument("--indirect", nargs="+", type=float, metavar="K", help="bounced-light scales to compare")
    parser.add_argument("--size", default="320x240")
    parser.add_argument("--spp", type=int, default=64)
    parser.add_argument("--out", help="the sheet's path (default: light_preview.png in the temporary directory)")
    parser.add_argument("--no-show", action="store_true", help="do not open the sheet")
    parser.add_argument("--save", action="store_true", help="write the one --rotation into the scene file")
    args = parser.parse_args(argv)
    if args.save and len(args.rotation) != 1:
        parser.error("--save takes exactly one --rotation")
    scene_path = pathlib.Path(args.scene)
    scene = load_scene(scene_path)
    lights = [item for item in scene.objects if item.kind == "light"]
    named = [item for item in lights if args.light in (None, item.name)]
    if len(named) != 1:
        parser.error("name the light with --light: " + ", ".join(item.name for item in lights) if named
                     else f"the scene has no light named {args.light!r}")
    sun_object = named[0]
    roll = sun_object.rotation[2]
    rotations = [list(sun_object.rotation)] + [[pitch, yaw, roll] for pitch, yaw in args.rotation]

    if args.save:
        text = scene_path.read_bytes().decode("utf-8")
        scene_path.write_bytes(save_rotation(text, sun_object.name, rotations[1]).encode("utf-8"))
        print(f"{scene_path}: {sun_object.name} {describe(rotations[1])}")
        return 0

    cameras = [item for item in scene.objects if item.kind == "camera" and args.camera in (None, item.name)]
    if not cameras or cameras[0].component.path is None:
        parser.error(f"no camera {args.camera or ''} with a path")
    camera = cameras[0].component
    width, height = map(int, args.size.split("x"))
    lens, near = camera.half_fov_short_tan, camera.near_z
    poses = path_poses(camera.path.animation, camera.path.node, args.at)

    source, job = source_for(scene, args.object, lit=False)
    traced_lights = [light for light in scene.lights if light["type"] != "ambient"]
    sun = sun_object.component
    shares = {}
    for pattern in args.share:
        chosen = [index for index, name in enumerate(source.names) if fnmatch.fnmatch(name, pattern)]
        if not chosen:
            parser.error(f"--share {pattern!r} names no material of the source")
        shares[pattern] = np.isin(source.tri_m, chosen)

    bounced = job.bake.indirect is not None and job.renderer.indirect
    scales = args.indirect if args.indirect is not None else [scene.indirect.intensity if bounced else 0.0]
    if any(k < 0 for k in scales):
        parser.error("--indirect must not be negative")
    # A path of depth 2 is direct light only; each bounce the bake traces adds one.
    depth = DIRECT_DEPTH + (job.bake.indirect.bounces if bounced else mitsuba_reference.DEFAULT_DEPTH - DIRECT_DEPTH)
    rows = []
    query = None
    rng = np.random.default_rng(1)
    for number, rotation in enumerate(rotations):
        sun["direction"] = toward(rotation).tolist()
        tracer = mitsuba_reference.prepare(source, traced_lights, job.settings.double_sided, keep_textures=True,
                                           albedo_boost=scene.indirect.albedo_boost)
        if query is None:
            query = RayQuery(source.p, source.tri_v, variant=tracer.mi.variant())
            samples = {pattern: area_samples(source, np.flatnonzero(mask), AREA_SAMPLES, rng)
                       for pattern, mask in shares.items()}
            hits = []
            for pose in poses:
                origin, direction = camera_rays(width, height, lens, pose[:3], pose[3:], 1)
                locations, rays, faces = query.first_hit(origin, direction)
                a, b, c = (source.p[source.tri_v[faces, k]] for k in range(3))
                normal = np.cross(b - a, c - a)
                normal /= np.maximum(np.linalg.norm(normal, axis=1, keepdims=True), 1e-12)
                hits.append((locations, normal, faces))
        direction = np.array(sun["direction"])
        line = [describe(rotation)]
        area = {p: in_sun(query, *samples[p], direction, job.bake.ray_offset).mean() for p in shares}
        line += ["%s area in sun %.1f%%" % (p, 100 * v) for p, v in area.items()]
        tiles = {k: [] for k in scales}
        for pose, (locations, normal, faces) in zip(poses, hits):
            direct, covered = tracer.trace(pose, width, height, lens, near, args.spp, max_depth=DIRECT_DEPTH)
            full = tracer.trace(pose, width, height, lens, near, args.spp, max_depth=depth)[0] if any(scales) else direct
            caption = []
            for pattern, mask in shares.items():
                seen = mask[faces]
                caption.append("%s %s" % (pattern, "%.0f%% lit" % (100 * in_sun(query, locations[seen], normal[seen],
                                direction, job.bake.ray_offset).mean()) if seen.any() else "not in view"))
            for k in scales:
                linear = direct + k * (full - direct)
                tiles[k].append((device_picture(linear, covered, scene.tonemap_white, camera.background),
                                 ", ".join(caption)))
        for k in scales:
            tag = "scene" if number == 0 else f"#{number}"
            print(f"{tag:6} " + "  ".join(line + ["indirect %g" % k]), flush=True)
            rows.append((tag, line[:1] + ["indirect %g" % k] + line[1:], tiles[k]))

    sheet = Image.new("RGB", (LABEL_WIDTH + width * len(poses), CAPTION + len(rows) * (height + CAPTION)), (24, 24, 24))
    draw = ImageDraw.Draw(sheet)
    for column, seconds in enumerate(args.at):
        draw.text((LABEL_WIDTH + column * width + 4, 2), f"{cameras[0].name} t={seconds:g}s", fill="white")
    for index, (tag, line, tiles) in enumerate(rows):
        y = CAPTION + index * (height + CAPTION)
        labels = [tag] + line[0].replace(" toward", "\ntoward").replace(
            " elevation", "\nelevation").split("\n") + line[1:]
        for k, label in enumerate(labels):
            draw.text((6, y + 4 + 14 * k), label, fill=(255, 220, 120) if k == 0 else "white")
        for column, (picture, caption) in enumerate(tiles):
            sheet.paste(Image.fromarray(picture), (LABEL_WIDTH + column * width, y))
            draw.text((LABEL_WIDTH + column * width + 4, y + height + 2), caption, fill="white")
    out = pathlib.Path(args.out) if args.out else pathlib.Path(tempfile.gettempdir()) / "light_preview.png"
    sheet.save(out)
    print(out)
    if not args.no_show and sys.stdout.isatty():
        if hasattr(os, "startfile"):
            os.startfile(out)
        else:
            subprocess.Popen(["xdg-open", str(out)])
    return 0


if __name__ == "__main__":
    sys.exit(main())
