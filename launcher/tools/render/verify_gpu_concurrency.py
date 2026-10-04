"""Rerunnable reference, fit and smoke determinism checks; writes scratch snapshots."""
import argparse
import hashlib
import json
import pathlib
import resource
import shutil
import subprocess
import sys
import time
import threading
import os

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(pathlib.Path(__file__).parent))


class MemorySamples:
    """Peak PSS per descendant and GPU samples while the foreground proof runs."""
    def __init__(self):
        self.stop = threading.Event()
        self.pss = {}
        self.rss = {}
        self.gpu = []

    def sample(self):
        while not self.stop.is_set():
            parents = {}
            for path in pathlib.Path("/proc").glob("[0-9]*/status"):
                try:
                    fields = dict(line.split(":", 1) for line in path.read_text().splitlines())
                    parents[int(path.parent.name)] = int(fields["PPid"])
                except (OSError, ValueError, KeyError):
                    continue
            descendants = {os.getpid()}
            while True:
                expanded = descendants | {pid for pid, parent in parents.items() if parent in descendants}
                if expanded == descendants:
                    break
                descendants = expanded
            for pid in descendants:
                try:
                    status = pathlib.Path(f"/proc/{pid}/status").read_text()
                    rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith("VmHWM:"))
                    self.rss[pid] = max(rss, self.rss.get(pid, 0))
                except (OSError, StopIteration):
                    pass
                try:
                    value = next(int(line.split()[1]) for line in pathlib.Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines()
                                 if line.startswith("Pss:"))
                    self.pss[pid] = max(value, self.pss.get(pid, 0))
                except (OSError, StopIteration):
                    pass
            try:
                sample = subprocess.check_output(["nvidia-smi", "--query-gpu=memory.used,utilization.gpu",
                                                  "--format=csv,noheader,nounits"], timeout=5).decode().splitlines()[0]
                self.gpu.append(tuple(int(value.strip()) for value in sample.split(",")))
            except (OSError, subprocess.SubprocessError, ValueError):
                pass
            self.stop.wait(0.2)

    def __enter__(self):
        self.thread = threading.Thread(target=self.sample)
        self.thread.start()
        return self

    def __exit__(self, *error):
        self.stop.set()
        self.thread.join()

    def report(self):
        return {"peak_pss_kib_by_pid": self.pss.copy(), "peak_rss_kib_by_pid": self.rss.copy(),
                "peak_gpu_mib": max((row[0] for row in self.gpu), default=None),
                "mean_gpu_utilization_percent": sum(row[1] for row in self.gpu) / len(self.gpu) if self.gpu else None}


def compare(left, right):
    import numpy as np
    names = sorted(path.relative_to(left) for path in left.rglob("*") if path.is_file())
    other = sorted(path.relative_to(right) for path in right.rglob("*") if path.is_file())
    if names != other:
        raise RuntimeError("snapshot file sets differ")
    differences = {}
    for name in names:
        a, b = left / name, right / name
        if a.read_bytes() != b.read_bytes():
            row = {"sha256": [hashlib.sha256(p.read_bytes()).hexdigest() for p in (a, b)]}
            if name.suffix == ".npy":
                row["maximum_absolute_delta"] = float(np.max(np.abs(np.load(a) - np.load(b))))
            elif name.suffix == ".txt" and name.name == "loss.txt":
                row["maximum_absolute_delta"] = float(np.max(np.abs(np.loadtxt(a) - np.loadtxt(b))))
            elif name.suffix == ".mesh":
                from r3d.lit_mesh import read_lit_mesh
                aa, bb = read_lit_mesh(a), read_lit_mesh(b)
                for field in ("pos", "rgb", "tris"):
                    x, y = getattr(aa, field), getattr(bb, field)
                    row[field + "_shape"] = [list(x.shape), list(y.shape)]
                    if x.shape == y.shape and x.size:
                        row[field + "_maximum_absolute_delta"] = float(np.max(np.abs(x.astype(float) - y.astype(float))))
                from scipy.spatial import cKDTree
                row["position_hausdorff_quantised"] = float(max(
                    cKDTree(aa.pos).query(bb.pos)[0].max(), cKDTree(bb.pos).query(aa.pos)[0].max()))
                def triangles(mesh):
                    return sorted(tuple(sorted((tuple(mesh.pos[index]), tuple(mesh.rgb[index])) for index in tri))
                                  for tri in mesh.tris)
                ta, tb = triangles(aa), triangles(bb)
                if len(ta) == len(tb):
                    pa = np.array([[corner[0] for corner in tri] for tri in ta])
                    pb = np.array([[corner[0] for corner in tri] for tri in tb])
                    row["triangle_geometry_identical"] = bool(np.array_equal(pa, pb))
                    if row["triangle_geometry_identical"]:
                        ca = np.array([[corner[1] for corner in tri] for tri in ta], dtype=float)
                        cb = np.array([[corner[1] for corner in tri] for tri in tb], dtype=float)
                        row["triangle_rgb_maximum_absolute_delta"] = float(np.max(np.abs(ca - cb)))
            differences[name.as_posix()] = row
    return {"files": len(names), "different": differences}


def main(samples):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("reference", "fit", "smoke", "compare"))
    parser.add_argument("--inputs", type=pathlib.Path, help="smoke reference input directory")
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--left", type=pathlib.Path)
    parser.add_argument("--right", type=pathlib.Path)
    parser.add_argument("--sky-list", action="store_true", help="reference proof: materialise sky directions before tracing")
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--size", type=int, nargs=2, help="reference slice width and height; defaults to input poses")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    from doc_stages import SCENE, RESULTS, current_stamp
    from r3d.fitted_variant import placed_variant, fit_point
    from r3d.import_settings import load_scene
    scene = load_scene(SCENE)
    job = placed_variant(scene, "sponza_fitted")
    result = {}
    if args.mode == "compare":
        result = compare(args.left, args.right)
    elif args.mode == "reference":
        from r3d.reference_render import source_for, render_poses
        if args.sky_list:
            from r3d import light as lighting
            original = lighting.unshadowed_count
            lighting.unshadowed_count = lambda intersector, origin, directions: original(intersector, origin, list(directions))
        from r3d.poses import read_poses
        start = time.monotonic()
        source, source_job = source_for(scene, job.object.name)
        result["source_cache_seconds"] = time.monotonic() - start
        result["source_peak_rss_kib"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
        width, height, lens, near, poses = read_poses(args.inputs / "train.txt")
        if args.size:
            width, height = args.size
        result["reference_size"] = [width, height]
        for label, workers in (("serial", 1), ("pool", args.workers)):
            directory = args.out / label
            directory.mkdir(exist_ok=True)
            start = time.monotonic()
            render_poses(source, source_job, scene, poses, width, height, lens, 4, directory, True, workers)
            result[label + "_seconds"] = time.monotonic() - start
        result["comparison"] = compare(args.out / "serial", args.out / "pool")
        if result["comparison"]["different"]:
            raise RuntimeError(json.dumps(result))
    elif args.mode == "fit":
        from r3d.process_budget import FitExecutor
        environment = pathlib.Path(sys.prefix)
        os.environ.setdefault("CUDA_HOME", str(environment))
        os.environ["PATH"] = str(environment / "bin") + os.pathsep + os.environ["PATH"]
        os.environ.setdefault("CPATH", str(environment / "targets/x86_64-linux/include"))
        os.environ.setdefault("LIBRARY_PATH", str(environment / "targets/x86_64-linux/lib") + ":" + str(environment / "lib"))
        os.environ.setdefault("CC", str(environment / "bin/x86_64-conda-linux-gnu-gcc"))
        os.environ.setdefault("CXX", str(environment / "bin/x86_64-conda-linux-gnu-g++"))
        for label in ("serial", "pool"):
            start = time.monotonic()
            if label == "serial":
                for index in range(2):
                    fit_point({}, args.out / label / str(index), SCENE, scene, job, args.inputs, True)
            else:
                with FitExecutor() as pool:
                    futures = [pool.submit(fit_point, {}, args.out / label / str(index), SCENE, scene, job, args.inputs, True)
                               for index in range(2)]
                    for future in futures:
                        future.result()
            result[label + "_seconds"] = time.monotonic() - start
        result["serial_spread"] = compare_fit(args.out / "serial/0", args.out / "serial/1")
        result["pool_spread"] = compare_fit(args.out / "pool/0", args.out / "pool/1")
        result["serial_vs_pool"] = compare_fit(args.out / "serial/0", args.out / "pool/0")
    else:
        for index in range(2):
            start = time.monotonic()
            subprocess.run(["sh", "launcher/tools/render/run_doc_gpu.sh", "--smoke"], cwd=ROOT, check=True)
            result[str(index) + "_seconds"] = time.monotonic() - start
            work = RESULTS / "work/gpu/smoke-" / current_stamp()
            snapshot = args.out / str(index)
            shutil.copytree(work / "references", snapshot / "references", dirs_exist_ok=True)
            shutil.copyfile(work / "smoke.mesh", snapshot / "smoke.mesh")
            shutil.copytree(RESULTS / "out/gpu-smoke", snapshot / "out", dirs_exist_ok=True)
        result["comparison"] = compare(args.out / "0", args.out / "1")
    result["resources"] = samples.report()
    result["peak_rss_kib"] = resource.getrusage(resource.RUSAGE_SELF).ru_maxrss
    (args.out / "report.json").write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps(result, indent=2))


def compare_fit(left, right):
    import tempfile
    with tempfile.TemporaryDirectory() as directory:
        root = pathlib.Path(directory)
        for source, name in ((left, "left"), (right, "right")):
            target = root / name
            target.mkdir()
            for path in (source / "fitted").iterdir():
                if path.suffix in (".mesh", ".txt"):
                    shutil.copyfile(path, target / path.name)
        return compare(root / "left", root / "right")


if __name__ == "__main__":
    if "--worker" in sys.argv:
        sys.argv.remove("--worker")
        main(MemorySamples())
    else:
        # Sampling stays outside the CPU worker that forks the reference pose pool.
        with MemorySamples() as samples:
            result = subprocess.run([sys.executable, __file__, *sys.argv[1:], "--worker"])
        if result.returncode == 0:
            report = pathlib.Path(sys.argv[sys.argv.index("--out") + 1]) / "report.json"
            data = json.loads(report.read_text())
            data["resources"] = samples.report()
            report.write_text(json.dumps(data, indent=2) + "\n")
        raise SystemExit(result.returncode)
