#!/usr/bin/env python3
"""Score scratch bakes of one flat mesh against a source reference.

    python launcher/tools/r3d/bake_fidelity.py SCENE.scene.toml --mesh NAME \\
        --script HOST_RENDER.sh --render-args "ARGS" --reference DIR --work DIR \\
        [--variant LABEL=SPEC ...]

Each variant re-lights the mesh's simplified geometry, which is baked once,
writes the result under --work (nothing tracked is touched), builds the
scene's host renderer with that mesh in place of the tracked one, renders
--render-args with a video, and scores the frames against the reference
images from reference_render.py with render_compare.py. Prints one table
sorted by mean error.

SPEC is comma-separated KEY=VALUE, every key optional:

    samples=fixed:N | auto:MIN:MAX:AREA   AREA is median, a number, or median*K
    sky=N                                 sky directions per face sample
    place=stratified | centroid           where a face's samples sit
    sun=disc | centre                     the sun's disc, or its middle only

No --variant scores the mesh as its import file declares it.
"""

import argparse
import os
import pathlib
import re
import subprocess
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import log  # noqa: E402
from r3d.import_settings import SettingsError, load_scene  # noqa: E402
from r3d.light import triangle_areas  # noqa: E402
from r3d.lit_mesh import write_lit_mesh  # noqa: E402
from r3d.mesh_import import REPO, bake_geometry, banner_lines, flat_colours  # noqa: E402

LAUNCHER = REPO / "launcher"
SCORE = re.compile(r"^mean: mean DeltaE76 ([\d.]+), p95 DeltaE76 ([\d.]+), luma SSIM ([\d.]+), edge DeltaE76 ([\d.]+), interior DeltaE76 ([\d.]+)",
                   re.M)


def parse_samples(text, median):
    """(samples, min, max, area) from `fixed:N` or `auto:MIN:MAX:AREA`."""
    fields = text.split(":")
    if fields[0] == "fixed" and len(fields) == 2:
        return int(fields[1]), 1, int(fields[1]), None
    if fields[0] == "auto" and len(fields) == 4:
        area = fields[3]
        if area.startswith("median"):
            area = median * float(area.partition("*")[2] or 1.0)
        return "auto", int(fields[1]), int(fields[2]), float(area)
    raise SettingsError(f"samples must be fixed:N or auto:MIN:MAX:AREA, not {text!r}")


def parse_spec(spec, declared, median):
    """The bake options for one variant: (face_samples, flat_colours knobs)."""
    options = dict(item.split("=", 1) for item in spec.split(",") if item)
    unknown = set(options) - {"samples", "sky", "place", "sun"}
    if unknown:
        raise SettingsError(f"unknown variant keys {sorted(unknown)}")
    samples = parse_samples(options["samples"], median) if "samples" in options else declared
    knobs = {}
    if "sky" in options:
        knobs["sky_rays"] = int(options["sky"])
    if "place" in options:
        knobs["placement"] = options["place"]
    if options.get("sun", "disc") not in ("disc", "centre"):
        raise SettingsError("sun must be disc or centre")
    knobs["sun_centre"] = options.get("sun") == "centre"
    return samples, knobs


def declared_samples(variant, median):
    """The import file's own face_samples, its area resolved to a number."""
    samples, low, high, area = variant.face_samples
    return samples, low, high, median if samples == "auto" and area is None else area


def build_host(script, mesh_source, mesh_file, out_dir):
    """The scene's host renderer with `mesh_file` built in place of
    `mesh_source` (relative to launcher/); returns its path."""
    env = dict(os.environ, RENDER_SCENE_SUBSTITUTE=f"{mesh_source}={mesh_file.as_posix()}")
    done = subprocess.run(["sh", script.as_posix(), "--build-only", "-o", out_dir.as_posix()], env=env, capture_output=True,
                          text=True, check=True)
    return pathlib.Path(done.stdout.split("built ", 1)[1].strip())


def score(args, host, work):
    """(mean, p95, ssim, edge, interior) of the frames `host` renders."""
    video = work / "frames.avi"
    subprocess.run([host.as_posix(), *args.render_args.split(), "-o", (work / "last.bmp").as_posix(), "--video", video.as_posix()],
                   check=True, capture_output=True)
    compare = LAUNCHER / "tools" / "render" / "render_compare.py"
    done = subprocess.run([sys.executable, compare.as_posix(), "--out", (work / "unused.png").as_posix(), "--reference-video",
                           video.as_posix(), pathlib.Path(args.reference).as_posix(), "--reference-scale", str(args.reference_scale),
                           "--heatmap-dir", (work / "heatmaps").as_posix()], check=True, capture_output=True, text=True)
    (work / "score.txt").write_text(done.stdout)
    return tuple(float(value) for value in SCORE.search(done.stdout).groups())


def table(rows):
    lines = ["| Variant | Mean dE76 | p95 dE76 | Luma SSIM | Edge dE76 | Interior dE76 |", "|---|---:|---:|---:|---:|---:|"]
    for label, values in sorted(rows, key=lambda row: row[1][0]):
        lines.append(f"| {label} | {values[0]:.3f} | {values[1]:.3f} | {values[2]:.4f} | {values[3]:.3f} | {values[4]:.3f} |")
    return "\n".join(lines)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("scene")
    parser.add_argument("--mesh", required=True, help="the flat variant to re-bake")
    parser.add_argument("--script", required=True, help="the scene's host-render script")
    parser.add_argument("--render-args", required=True, help="the host renderer's arguments, without -o and --video")
    parser.add_argument("--reference", required=True, help="reference_render.py's output directory")
    parser.add_argument("--reference-scale", type=int, default=2, help="host render pixels per reference pixel")
    parser.add_argument("--work", required=True, help="scratch directory for bakes, renders and heatmaps")
    parser.add_argument("--variant", action="append", default=[], metavar="LABEL=SPEC")
    args = parser.parse_args(argv)
    path = pathlib.Path(args.scene).resolve()
    scene = load_scene(path)
    jobs = [item for item in scene.renderers if item.variant.name == args.mesh and item.variant.face_samples]
    if not jobs:
        parser.error(f"{args.mesh!r} is not a flat mesh of {path.name}")
    settings, variant = jobs[0].settings, jobs[0].variant
    log(f"geometry of {variant.name}")
    geometry = bake_geometry(settings, variant, scene)
    median = float(np.median(triangle_areas(geometry.positions, geometry.tris)))
    tracked = (settings.out_dir / f"{variant.name}_mesh_generated.c").resolve().relative_to(LAUNCHER).as_posix()
    work = pathlib.Path(args.work).resolve()
    rows = []
    for item in args.variant or ["declared="]:
        label, _, spec = item.partition("=")
        samples, knobs = parse_spec(spec, declared_samples(variant, median), median)
        out = work / label
        out.mkdir(parents=True, exist_ok=True)
        log(f"variant {label}")
        face_rgb = flat_colours(settings, scene, geometry, samples, **knobs)
        write_lit_mesh(out, variant.name, geometry.positions, None, geometry.tris, geometry.tri_double,
                       banner_lines(path, settings, variant), face_rgb=face_rgb, **geometry.scale)
        host = build_host(pathlib.Path(args.script).resolve(), tracked, out / f"{variant.name}_mesh_generated.c", out / "host")
        rows.append((label, score(args, host, out)))
        print(f"{label}: mean dE76 {rows[-1][1][0]:.3f}", flush=True)
    print(table(rows))
    (work / "table.md").write_text(table(rows) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
