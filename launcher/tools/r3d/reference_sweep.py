#!/usr/bin/env python3
"""Measure the path-traced reference's noise, depth bias, time and GPU memory on one pose.

    python launcher/tools/r3d/reference_sweep.py SCENE.scene.toml --poses poses.txt --pose-index N --out DIR
        [--sky hosek-wilkie] [--spp 64 256 1024] [--depths 12 24] [--seeds 4]

Per spp it renders `--seeds` independent seeds and reports the relative per-pixel noise of the linear image and the
mean CIE76 dE between two device pictures of the same setting; per depth it reports how much a deeper cap changes
the picture against the shallowest cap at the same seeds. Times separate the export, the first (cold) render,
which compiles kernels, and the warm ones; GPU memory is sampled from nvidia-smi throughout each block. Writes sweep.json and
sweep.md to the output folder.
"""

import argparse
import ctypes
import json
import os
import pathlib
import subprocess
import sys
import threading
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "render"))

from r3d.process_budget import PeakSampler
from r3d import mitsuba_reference
from r3d.import_settings import load_scene
from r3d.poses import read_poses
from r3d.reference_render import device_picture, source_for
from render_compare import lab


def available_memory_gib():
    """Host memory a new allocation can use now, in GiB."""
    if sys.platform == "win32":

        class Status(ctypes.Structure):
            _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong), ("phys", ctypes.c_ulonglong),
                        ("avail", ctypes.c_ulonglong), ("page", ctypes.c_ulonglong), ("page_avail", ctypes.c_ulonglong),
                        ("virt", ctypes.c_ulonglong), ("virt_avail", ctypes.c_ulonglong), ("ext", ctypes.c_ulonglong)]

        status = Status(length=ctypes.sizeof(Status))
        ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
        return status.avail / 2**30
    for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
        if line.startswith("MemAvailable:"):
            return int(line.split()[1]) / 2**20
    raise RuntimeError("cannot read the available memory")


def require_memory(floor, stage):
    free = available_memory_gib()
    print(f"{stage}: {free:.1f} GiB host memory available", flush=True)
    if free < floor:
        raise SystemExit(f"{stage}: {free:.1f} GiB available is below the {floor} GiB floor")


def guard_memory(floor):
    """Stop the whole process the moment host memory available falls below `floor` GiB, even in the middle of a load."""

    def watch():
        while True:
            if available_memory_gib() < floor:
                print(f"host memory available fell below {floor} GiB: stopping", flush=True)
                os._exit(3)
            time.sleep(0.25)

    threading.Thread(target=watch, daemon=True).start()


class VramPeak:
    """Device-wide GPU memory in MiB, sampled throughout the block."""

    def __init__(self, probe=None):
        self.sampler = PeakSampler(lambda: ((probe or self.used)(),), interval=0.1)
        self.peak = None

    @staticmethod
    def used():
        """GPU memory in use in MiB, or None without an NVIDIA GPU."""
        try:
            out = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"], capture_output=True,
                                 text=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            return None
        return max(int(line) for line in out.split())

    def __enter__(self):
        self.sampler.__enter__()
        self.peak = self.sampler.peaks[0]
        return self

    def __exit__(self, *error):
        self.sampler.__exit__(*error)
        self.peak = self.sampler.peaks[0]


def timed(function):
    start = time.perf_counter()
    result = function()
    return result, time.perf_counter() - start


def delta_e(a, b):
    """Mean CIE76 dE between two 8-bit device pictures."""
    return float(np.linalg.norm(lab(np, a / 255.0) - lab(np, b / 255.0), axis=-1).mean())


def relative_noise(images):
    """RMS over pixels and channels of the seed-to-seed standard deviation, as a share of the mean radiance."""
    stack = np.stack(images)
    mean = stack.mean()
    return float(np.sqrt(stack.var(axis=0, ddof=1).mean()) / mean) if mean else 0.0


def measure(trace, depths, spps, depth_spp, seeds, picture, before=lambda stage: None):
    """The sweep's measurements of `trace(spp, seed, depth) -> (linear, covered)`: for the first depth every spp, for
    each later depth only `depth_spp`, each over seeds 1..`seeds`; `picture(linear, covered)` makes the 8-bit
    device picture the dE is taken on. Returns the `depths`, `cold_seconds` and `cold_spp` of the result."""
    kept, result = {}, {}
    for depth in depths:
        entry = {"spp": {}}
        result[depth] = entry
        sweep = spps if depth == depths[0] else []
        for spp in sorted(set(sweep) | {depth_spp}):
            before(f"before depth {depth}, {spp} spp")
            images, times = [], []
            with VramPeak() as peak:
                for seed in range(1, seeds + 1):
                    image, seconds = timed(lambda: trace(spp, seed, depth))
                    images.append(image)
                    times.append(seconds)
            kept[(depth, spp)] = images
            entry["spp"][spp] = {"seconds": times, "peak_mib": peak.peak}
            print(f"depth {depth} spp {spp}: {', '.join('%.2f s' % t for t in times)}, peak {mib(peak.peak)}", flush=True)
    first = result[depths[0]]["spp"][min(result[depths[0]]["spp"])]
    cold = {"cold_seconds": first["seconds"][0], "cold_spp": min(result[depths[0]]["spp"])}
    # The first trace of the first setting compiled the kernels: it is the cold time, never a warm one.
    first["seconds"] = first["seconds"][1:]
    for depth, entry in result.items():
        for spp, cell in entry["spp"].items():
            images = kept[(depth, spp)]
            linear = [item[0] for item in images]
            cell["warm_seconds_mean"] = float(np.mean(cell["seconds"]))
            cell["relative_noise"] = relative_noise(linear)
            cell["seed_pair_delta_e"] = delta_e(picture(*images[0]), picture(*images[1]))
            cell["mean_radiance"] = float(np.mean(linear))
    base = depths[0]
    for depth in depths[1:]:
        pairs = [(kept[(base, depth_spp)][i], kept[(depth, depth_spp)][i]) for i in range(seeds)]
        result[depth]["against_depth_%d" % base] = {
            "mean_delta_e": float(np.mean([delta_e(picture(*a), picture(*b)) for a, b in pairs])),
            "radiance_ratio": float(np.mean([b[0].mean() / a[0].mean() for a, b in pairs]))}
    return {"depths": result, **cold}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("scene")
    parser.add_argument("--object")
    parser.add_argument("--poses", required=True)
    parser.add_argument("--pose-index", type=int, required=True)
    parser.add_argument("--out", required=True)
    parser.add_argument("--spp", type=int, nargs="+", default=[64, 256, 1024])
    parser.add_argument("--depths", type=int, nargs="+", default=[12, 24])
    parser.add_argument("--depth-spp", type=int, default=256, help="spp of the depth comparison")
    parser.add_argument("--seeds", type=int, default=4)
    mitsuba_reference.add_options(parser)
    parser.add_argument("--min-free-gib", type=float, default=3.0, help="stop when host memory available is lower")
    args = parser.parse_args(argv)
    if args.seeds < 2:
        parser.error("--seeds must be at least 2")
    scene = load_scene(args.scene)
    width, height, lens, near, poses = read_poses(args.poses)
    pose = poses[args.pose_index]
    sky = mitsuba_reference.sky_from(args)
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    background = scene.camera.component.background
    baseline = VramPeak.used()

    def picture(linear, covered):
        return device_picture(linear, covered, scene.tonemap_white, background).astype(float)

    require_memory(args.min_free_gib, "before loading the source")
    guard_memory(args.min_free_gib)
    source, job = source_for(scene, args.object, lit=False)
    result = {"scene": str(args.scene), "pose_index": args.pose_index, "size": [width, height], "sky": sky, "seeds": args.seeds,
              "variant": args.variant, "base_depth": args.depths[0], "triangles": int(len(source.tri_v)), "vram_baseline_mib": baseline}
    require_memory(args.min_free_gib, "before the export")
    with VramPeak() as export_peak:
        path, export_seconds = timed(lambda: mitsuba_reference.prepare(source, scene.lights, job.settings.double_sided, sky,
                                                                      args.variant))
    result.update(export_seconds=export_seconds, export_peak_mib=export_peak.peak)
    result.update(measure(lambda spp, seed, depth: path.trace(pose, width, height, lens, near, spp, seed, depth), args.depths,
                          args.spp, args.depth_spp, args.seeds, picture,
                          lambda stage: require_memory(args.min_free_gib, stage)))
    (out / "sweep.json").write_text(json.dumps(result, indent=2))
    (out / "sweep.md").write_text(table(result))
    print(table(result))
    return 0


def mib(value):
    return "n/a" if value is None else f"{value} MiB"


def table(result):
    base = result["base_depth"]
    lines = ["| depth | spp | relative noise | seed-pair mean dE76 | warm seconds | peak MiB |", "|---|---|---|---|---|---|"]
    for depth, entry in result["depths"].items():
        for spp, cell in sorted(entry["spp"].items()):
            lines.append(f"| {depth} | {spp} | {cell['relative_noise']:.4f} | {cell['seed_pair_delta_e']:.3f} | "
                         f"{cell['warm_seconds_mean']:.2f} | {mib(cell['peak_mib']).replace(' MiB', '')} |")
    lines += ["", "| depth | against depth %d: mean dE76 | radiance ratio |" % base, "|---|---|---|"]
    for depth, entry in result["depths"].items():
        if "against_depth_%d" % base in entry:
            cell = entry["against_depth_%d" % base]
            lines.append(f"| {depth} | {cell['mean_delta_e']:.3f} | {cell['radiance_ratio']:.4f} |")
    lines += ["", f"export: {result['export_seconds']:.2f} s, peak GPU memory in use {mib(result['export_peak_mib'])}",
              f"cold first render ({result['cold_spp']} spp): {result['cold_seconds']:.2f} s",
              f"GPU memory in use before the run: {mib(result['vram_baseline_mib'])}"]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.exit(main())
