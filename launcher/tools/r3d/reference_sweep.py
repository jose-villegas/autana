#!/usr/bin/env python3
"""Measure the path-traced reference's noise, depth bias, time and GPU memory on one pose.

    python launcher/tools/r3d/reference_sweep.py SCENE.scene.toml --poses poses.txt --pose-index N --out DIR
        [--sky hosek-wilkie] [--spp 64 256 1024] [--depths 12 24] [--seeds 4]

Per spp it renders `--seeds` independent seeds and reports the relative per-pixel noise of the linear image and the
mean CIE76 dE between two device pictures of the same setting; per depth it reports how much a deeper cap changes
the picture against the shallowest cap at the same seeds. Times separate the export, the first (cold) render,
which compiles kernels, and the warm ones; peak GPU memory is sampled from nvidia-smi. Writes sweep.json and
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
    """The most GPU memory in use, in MiB, while the block runs, sampled from nvidia-smi."""

    def __init__(self, interval=0.1):
        self.interval, self.peak, self._stop = interval, 0, threading.Event()

    @staticmethod
    def used():
        out = subprocess.run(["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"], capture_output=True,
                             text=True, check=True).stdout
        return max(int(line) for line in out.split())

    def __enter__(self):
        def poll():
            while not self._stop.is_set():
                self.peak = max(self.peak, self.used())
                time.sleep(self.interval)

        self.peak = self.used()
        self._thread = threading.Thread(target=poll, daemon=True)
        self._thread.start()
        return self

    def __exit__(self, *_):
        self._stop.set()
        self._thread.join()
        self.peak = max(self.peak, self.used())


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
    return float(np.sqrt(stack.var(axis=0, ddof=1).mean()) / stack.mean())


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
    parser.add_argument("--sky", choices=("hosek-wilkie",))
    parser.add_argument("--turbidity", type=float, default=3.0)
    parser.add_argument("--ground-albedo", type=float, default=0.3)
    parser.add_argument("--variant")
    parser.add_argument("--min-free-gib", type=float, default=3.0, help="stop when host memory available is lower")
    args = parser.parse_args(argv)
    if args.seeds < 2:
        parser.error("--seeds must be at least 2")
    scene = load_scene(args.scene)
    width, height, lens, near, poses = read_poses(args.poses)
    pose = poses[args.pose_index]
    sky = {"turbidity": args.turbidity, "albedo": args.ground_albedo} if args.sky else None
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
              "variant": args.variant, "base_depth": args.depths[0], "triangles": int(len(source.tri_v)), "vram_baseline_mib": baseline, "depths": {}}
    kept = {}
    require_memory(args.min_free_gib, "before the export")
    with VramPeak() as export_peak:
        path, export_seconds = timed(lambda: mitsuba_reference.prepare(source, scene.lights, job.settings.double_sided, pose, width,
                                                                      height, lens, near, args.depths[0], sky, args.variant,
                                                                      release_textures=True))
    result.update(export_seconds=export_seconds, export_peak_mib=export_peak.peak)
    for depth in args.depths:
        entry = {"spp": {}}
        result["depths"][depth] = entry
        sweep = args.spp if depth == args.depths[0] else []
        for spp in sorted(set(sweep) | {args.depth_spp}):
            require_memory(args.min_free_gib, f"before depth {depth}, {spp} spp")
            images, times = [], []
            with VramPeak() as peak:
                for seed in range(args.seeds):
                    (linear, covered), seconds = timed(lambda: path.trace(spp, seed + 1, depth))
                    images.append((linear, covered))
                    times.append(seconds)
            kept[(depth, spp)] = images
            entry["spp"][spp] = {"seconds": times, "peak_mib": peak.peak}
            print(f"depth {depth} spp {spp}: {', '.join('%.2f s' % t for t in times)}, peak {peak.peak} MiB", flush=True)
        # The first trace of the first setting compiled the kernels: report it as cold, never as warm.
        if depth == args.depths[0]:
            first = entry["spp"][min(entry["spp"])]
            result["cold_seconds"] = first["seconds"][0]
            result["cold_spp"] = min(entry["spp"])
            first["seconds"] = first["seconds"][1:]
    for depth, entry in result["depths"].items():
        for spp, cell in entry["spp"].items():
            images = kept[(depth, spp)]
            linear = [item[0] for item in images]
            pictures = [picture(*item) for item in images]
            cell["warm_seconds_mean"] = float(np.mean(cell["seconds"]))
            cell["relative_noise"] = relative_noise(linear)
            cell["seed_pair_delta_e"] = delta_e(pictures[0], pictures[1])
            cell["mean_radiance"] = float(np.mean(linear))
    base = args.depths[0]
    for depth in args.depths[1:]:
        deltas = [delta_e(picture(*kept[(base, args.depth_spp)][i]), picture(*kept[(depth, args.depth_spp)][i]))
                  for i in range(args.seeds)]
        ratio = np.mean([kept[(depth, args.depth_spp)][i][0].mean() / kept[(base, args.depth_spp)][i][0].mean()
                         for i in range(args.seeds)])
        result["depths"][depth]["against_depth_%d" % base] = {"mean_delta_e": float(np.mean(deltas)), "radiance_ratio": float(ratio)}
    (out / "sweep.json").write_text(json.dumps(result, indent=2))
    (out / "sweep.md").write_text(table(result))
    print(table(result))
    return 0


def table(result):
    base = result["base_depth"]
    lines = ["| depth | spp | relative noise | seed-pair mean dE76 | warm seconds | peak MiB |", "|---|---|---|---|---|---|"]
    for depth, entry in result["depths"].items():
        for spp, cell in sorted(entry["spp"].items()):
            lines.append(f"| {depth} | {spp} | {cell['relative_noise']:.4f} | {cell['seed_pair_delta_e']:.3f} | "
                         f"{cell['warm_seconds_mean']:.2f} | {cell['peak_mib']} |")
    lines += ["", "| depth | against depth %d: mean dE76 | radiance ratio |" % base, "|---|---|---|"]
    for depth, entry in result["depths"].items():
        if "against_depth_%d" % base in entry:
            cell = entry["against_depth_%d" % base]
            lines.append(f"| {depth} | {cell['mean_delta_e']:.3f} | {cell['radiance_ratio']:.4f} |")
    lines += ["", f"export: {result['export_seconds']:.2f} s, peak GPU memory in use {result['export_peak_mib']} MiB",
              f"cold first render ({result['cold_spp']} spp): {result['cold_seconds']:.2f} s",
              f"GPU memory in use before the run: {result['vram_baseline_mib']} MiB"]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    sys.exit(main())
