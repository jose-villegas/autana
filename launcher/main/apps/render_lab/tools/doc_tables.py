"""Write CPU fidelity tables from the doc-image run's comparison logs."""
import argparse
import pathlib
import re
import sys

from PIL import Image

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools/render"))
from render_compare import read_video, reference_video
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools"))
from r3d.bake_fidelity import table

SCORE = re.compile(r"^(.+): mean DeltaE76 ([\d.]+), p95 DeltaE76 ([\d.]+), luma SSIM (-?[\d.]+), "
                   r"edge DeltaE76 ([\d.]+), interior DeltaE76 ([\d.]+)", re.M)


def scores(path):
    found = {label: tuple(float(value) for value in values) for label, *values in
             SCORE.findall(path.read_text(encoding="utf-8"))}
    if not found:
        raise ValueError(f"no fidelity scores in {path}")
    return found


def write_tables(work, out):
    out.mkdir(parents=True, exist_ok=True)
    indirect = scores(work / "indirect-compare.log")
    smooth = indirect["two bounces"]
    full = scores(work / "committed-smooth-compare.log")["frames mean"]
    lite = scores(work / "sponza-lite-compare.log")["frames mean"]
    flat = scores(work / "fidelity-compare.log")["frames mean"]
    (out / "sponza-fidelity.md").write_text(table([
        ("Full smooth", full), ("Lite smooth", lite), ("Flat, committed", flat)]) + "\n", encoding="utf-8")
    smooth_frames = work / "smooth-reference"
    smooth_frames.mkdir(exist_ok=True)
    _, frames = read_video(work / "committed-smooth.avi")
    for index, frame in enumerate(frames):
        Image.fromarray(frame).save(smooth_frames / f"{index:04d}.png")
    _, pair = reference_video(work / "fidelity-flat.avi", smooth_frames, scale=1)
    (out / "sponza-flat-smooth.md").write_text(table([
        ("Flat against smooth, fidelity poses", (pair.mean_delta_e, pair.p95_delta_e, pair.ssim_luma,
                                                pair.edge_delta_e, pair.interior_delta_e))]) + "\n", encoding="utf-8")
    (out / "sponza-flat-sampling.md").write_bytes((work / "sampling/table.md").read_bytes())
    (out / "sponza-indirect.md").write_text(table([
        ("Full smooth, direct light", indirect["direct light only"]), ("Full smooth, indirect light", smooth),
        ("Lite smooth, direct light", scores(work / "direct-lite-compare.log")["frames mean"]),
        ("Lite smooth, indirect light", scores(work / "smooth-lite-compare.log")["frames mean"]),
        ("Flat, direct light", scores(work / "direct-flat-compare.log")["frames mean"]),
        ("Flat, indirect light", scores(work / "smooth-flat-compare.log")["frames mean"])]) + "\n", encoding="utf-8")
    look = scores(work / "indirect-look.log")
    lines = ["| Look | Mean dE76, physical | Mean dE76, own | p95, physical | SSIM, physical |",
             "|---|---:|---:|---:|---:|"]
    direct = indirect["direct light only"]
    lines.append(f"| Direct light only | {direct[0]:.3f} | | {direct[1]:.3f} | {direct[2]:.4f} |")
    for label in ("intensity 1", "intensity 2", "intensity 3", "albedo boost 2"):
        physical, own = look[label], look[label + " (own reference)"]
        lines.append(f"| {label} | {physical[0]:.3f} | {own[0]:.3f} | {physical[1]:.3f} | {physical[2]:.4f} |")
    (out / "sponza-indirect-look.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    args = parser.parse_args()
    write_tables(args.work, args.out)
