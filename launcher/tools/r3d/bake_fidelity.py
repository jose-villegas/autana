#!/usr/bin/env python3
"""Score scratch bakes of one flat mesh against a source reference.

    python launcher/tools/r3d/bake_fidelity.py SCENE.scene.toml --mesh NAME \\
        --script HOST_RENDER.sh --render-args "ARGS" --reference DIR --work DIR \\
        [--variant LABEL=SPEC ...]

--host EXECUTABLE reuses an existing renderer instead of building --script.

Each variant re-lights the mesh's simplified geometry, which is baked once,
writes the result under --work (nothing tracked is touched), packs it in
place of the tracked mesh for the scene's host renderer (AUTANA_ASSET_DIR),
renders --render-args with a video, and scores the frames against the reference
images from reference_render.py with render_compare.py. Prints one table
sorted by mean error.

SPEC is comma-separated KEY=VALUE, every key optional:

    samples=fixed:N | auto:MIN:MAX:AREA   AREA is median, a number, or median*K
    sky=N                                 sky directions per face sample
    place=stratified | centroid           where a face's samples sit

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
from r3d.build_pack import pack_bytes, write_packs  # noqa: E402
from r3d.lit_mesh import write_lit_mesh  # noqa: E402
from r3d.mesh_import import REPO, bake_geometry, flat_colours  # noqa: E402

LAUNCHER = REPO / "launcher"
SCORE = re.compile(r"^frames mean: mean DeltaE76 ([\d.]+), p95 DeltaE76 ([\d.]+), luma SSIM ([\d.]+), edge DeltaE76 ([\d.]+), interior DeltaE76 ([\d.]+)",
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
    unknown = set(options) - {"samples", "sky", "place"}
    if unknown:
        raise SettingsError(f"unknown variant keys {sorted(unknown)}")
    samples = parse_samples(options["samples"], median) if "samples" in options else declared
    knobs = {}
    if "sky" in options:
        knobs["sky_rays"] = int(options["sky"])
    if "place" in options:
        knobs["placement"] = options["place"]
    return samples, knobs


def declared_samples(renderer, median):
    """The renderer's face samples, with its area resolved to a number."""
    samples, low, high, area = renderer.face_samples
    return samples, low, high, median if samples == "auto" and area is None else area


def build_host(script, out_dir, scene):
    """The scene's host renderer; returns its path."""
    inputs = [scene.as_posix()]
    done = subprocess.run(["sh", script.as_posix(), *inputs, "--build-only", "-o", out_dir.as_posix()], capture_output=True, text=True,
                          check=True)
    return pathlib.Path(done.stdout.split("built ", 1)[1].strip())


def write_assets(name, mesh_file, out, scene):
    """Pack the supplied scene with one scratch mesh;
    returns out/assets."""
    assets = out / "assets"
    write_packs(assets, pack_bytes([scene], [f"{name}={mesh_file}"]))
    return assets


def write_variant(job, scene, geometry, spec, out):
    """Bakes `spec` (see the module docstring) over `geometry` into
    out/<name>.mesh; returns its path."""
    renderer = job.renderer
    median = float(np.median(triangle_areas(geometry.positions, geometry.tris)))
    samples, knobs = parse_spec(spec, declared_samples(renderer, median), median)
    face_rgb = flat_colours(job, scene, geometry, samples, **knobs)
    out.mkdir(parents=True, exist_ok=True)
    write_lit_mesh(out, renderer.variant.name, geometry.positions, None, geometry.tris, geometry.tri_double,
                   face_rgb=face_rgb, **geometry.scale)
    return out / f"{renderer.variant.name}.mesh"


def score(args, host, assets, work):
    """(mean, p95, ssim, edge, interior) of the frames `host` renders from the packs in `assets`."""
    video = work / "frames.avi"
    subprocess.run([host.as_posix(), *args.render_args.split(), "-o", (work / "last.bmp").as_posix(), "--video", video.as_posix()],
                   check=True, capture_output=True, env={**os.environ, "AUTANA_ASSET_DIR": assets.as_posix()})
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
    parser.add_argument("--mesh", required=True, help="the scene object, a flat renderer, to re-bake")
    host_group = parser.add_mutually_exclusive_group(required=True)
    host_group.add_argument("--host", help="an already built host renderer")
    host_group.add_argument("--script", help="the scene's host-render script")
    parser.add_argument("--render-args", required=True, help="the host renderer's arguments, without -o and --video")
    parser.add_argument("--reference", required=True, help="reference_render.py's output directory")
    parser.add_argument("--reference-scale", type=int, default=2, help="host render pixels per reference pixel")
    parser.add_argument("--work", required=True, help="scratch directory for bakes, renders and heatmaps")
    parser.add_argument("--variant", action="append", default=[], metavar="LABEL=SPEC")
    args = parser.parse_args(argv)
    path = pathlib.Path(args.scene).resolve()
    scene = load_scene(path)
    jobs = [item for item in scene.renderers if item.object.name == args.mesh and item.renderer.face_samples]
    if not jobs:
        parser.error(f"{args.mesh!r} is not a flat mesh of {path.name}")
    job = jobs[0]
    variant = job.renderer.variant
    log(f"geometry of {variant.name}")
    geometry = bake_geometry(job, scene)
    work = pathlib.Path(args.work).resolve()
    host = pathlib.Path(args.host).resolve() if args.host else build_host(pathlib.Path(args.script).resolve(), work / "host", path)
    rows = []
    for item in args.variant or ["declared="]:
        label, _, spec = item.partition("=")
        out = work / label
        log(f"variant {label}")
        mesh_file = write_variant(job, scene, geometry, spec, out)
        rows.append((label, score(args, host, write_assets(job.asset_name, mesh_file, out, pathlib.Path(args.scene)), out)))
        print(f"{label}: mean dE76 {rows[-1][1][0]:.3f}", flush=True)
    print(table(rows))
    (work / "table.md").write_text(table(rows) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
