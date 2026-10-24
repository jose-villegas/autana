#!/usr/bin/env python3
"""Draw each annotated error curve from captures with explicit execution provenance."""
import argparse
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(ROOT / "launcher/tools"), str(ROOT / "launcher/tools/perf")]
import fast_paths
import math_bench_table as bench
from docs.plot_style import FAINT, SLOTS, style_axes, subplots, save


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", type=Path, default=bench.DATA / "math_bench_capture.txt")
    parser.add_argument("--out", type=Path, default=ROOT / "docs/images/math")
    args = parser.parse_args(argv)
    try:
        items = fast_paths.annotations()
        text = args.capture.read_text(encoding="utf-8")
        bins = bench.parse_bins(text)
        if not bins:
            raise ValueError("capture has no MATHERRORBIN rows; capture math_numbers on the board first")
        if not bench.parse_target(text):
            raise ValueError("error curves require a hardware MATHTARGET capture record")
        rows, _ = bench.read_fpu(bench.DATA / "math_bench_fpu_ops.txt")
        _, bins, sources = fast_paths.error_evidence(ROOT, items, rows, text)
        done = set()
        for item in items:
            if item.key in done or (item.key, item.kind) not in bins:
                continue
            done.add(item.key)
            points = sorted(bins[item.key, item.kind])
            x, y, _ = zip(*points)
            fig, axis = subplots(1, 1, (7, 3.6))
            axis.plot(x, y, color=SLOTS[0], label="Binned maximum")
            axis.axhline(item.limit, color=SLOTS[1], linestyle="--", label=f"{item.limit_name} = {item.limit:g}")
            if min(x) > 0 and max(x) / min(x) >= 2:
                axis.set_xscale("log", base=2)
            axis.set_ylim(bottom=0, top=max(item.limit, max(y), 1.0 if item.limit == 0 else 1e-30) * 1.15)
            axis.set_xlabel("Input bin centre")
            axis.set_ylabel(f"Error ({item.unit})")
            axis.grid(color=FAINT)
            style_axes(axis, f"{item.key} ({sources[item.key, item.kind]})")
            axis.legend(loc="upper center", bbox_to_anchor=(0.5, -0.24), fontsize="small")
            save(fig, args.out / f"fast-path-{item.key}.png")
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f"fast_path_figures: {error}\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
