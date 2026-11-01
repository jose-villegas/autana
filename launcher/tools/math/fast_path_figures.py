#!/usr/bin/env python3
"""Draw each annotated error curve from captures with explicit execution provenance."""
import argparse
import math
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "launcher/tools"), str(ROOT / "launcher/tools/perf")]
import fast_paths
import math_bench_table as bench
from docs.plot_style import FAINT, SLOTS, style_axes, subplots, save


ERROR_ZOOM_FRACTION = 0.75
ERROR_HEADROOM = 1.15


def _configure_axis(axis, points, mode=None):
    nonzero = [point for point in sorted(points) if point[1] != 0]
    x, _, _ = zip(*(nonzero or sorted(points)))
    periodic = mode in ("period", "phase")
    if not periodic and min(x) > 0 and max(x) / min(x) >= 2:
        axis.set_xscale("log", base=2)
    axis.set_xlim(min(x), max(x))
    label = "Phase (turns)" if mode == "phase" else "Input bin centre"
    if mode == "period":
        period_end = 2 ** math.ceil(math.log2(max(point[0] for point in points)))
        label = f"Input mantissa (every input folded into [1, {period_end}))"
    axis.set_xlabel(label)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=bench.DATA / "math_bench_capture.txt")
    parser.add_argument("--out", type=Path, default=ROOT / "docs/images/math")
    args = parser.parse_args(argv)
    try:
        items = [item for item in fast_paths.annotations() if item.unit != "bit"]
        text = args.capture.read_text(encoding="utf-8")
        bins = bench.parse_bins(text)
        if not bins:
            raise ValueError("capture has no MATHERRORBIN rows; capture math_numbers on the board first")
        if not bench.parse_target(text):
            raise ValueError("error curves require a hardware MATHTARGET capture record")
        rows, _ = bench.read_fpu(bench.DATA / "math_bench_fpu_ops.txt")
        errors, bins, sources = fast_paths.error_evidence(ROOT, items, rows, text)
        axes = bench.parse_bin_axes(text)
        host = ROOT / fast_paths.HOST_CAPTURE
        if host.exists():
            axes.update(bench.parse_bin_axes(host.read_text(encoding="utf-8")))
        done = set()
        for item in items:
            identity = item.key, item.kind
            if item.key in done or identity not in bins or float(errors[identity].split()[0]) == 0:
                continue
            done.add(item.key)
            points = sorted(bins[item.key, item.kind])
            x, y, _ = zip(*points)
            fig, axis = subplots(1, 1, (7, 3.6))
            axis.plot(x, y, color=SLOTS[0], label="Binned maximum")
            zoom = float(errors[identity].split()[0]) < item.limit * ERROR_ZOOM_FRACTION
            limit_label = f"{item.limit_name} = {fast_paths.format_limit(item.limit)}"
            if not zoom:
                axis.axhline(item.limit, color=SLOTS[1], linestyle="--", label=limit_label)
            _configure_axis(axis, points, axes.get((item.key, item.kind)))
            axis.set_ylim(bottom=0, top=(max(y) if zoom else max(item.limit, max(y))) * ERROR_HEADROOM)
            unit = "ulp from the exact value" if item.unit == "ulp" and axes.get((item.key, item.kind)) == "period" else item.unit
            axis.set_ylabel(f"Error ({unit})")
            axis.grid(color=FAINT)
            title = f"{item.key} ({sources[identity]})"
            if zoom:
                title += f"\nLimit: {limit_label}"
            style_axes(axis, title)
            axis.legend(loc="upper center", bbox_to_anchor=(0.5, -0.24), fontsize="small")
            save(fig, args.out / f"fast-path-{item.key}.png")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"fast_path_figures: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
