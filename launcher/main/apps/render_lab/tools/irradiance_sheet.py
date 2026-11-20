#!/usr/bin/env python3
"""Compose reference, host render and masked dE76 rows from pose render folders."""
import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image
from scipy.ndimage import binary_erosion

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figures import save_png
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from render_compare import delta_e76, reference_heatmap_from_error, still_sheet
from r3d.skin_reference import MASK_THRESHOLD, REFERENCE_PICTURE, SKIN_COVERAGE


def comparison_row(reference, ours, coverage, label):
    """Labelled panels and mean/p95 over one pixel of erosion of the skin mask."""
    mask = binary_erosion(coverage > MASK_THRESHOLD, iterations=1)
    if mask.shape != (reference.height, reference.width):
        raise ValueError("skin mask and reference sizes differ")
    if not mask.any():
        raise ValueError("empty eroded skin mask")
    error = delta_e76(ours, reference)
    chosen = error[mask]
    stats = {"mean": float(chosen.mean()), "p95": float(np.percentile(chosen, 95)), "pixels": int(mask.sum())}
    heat = reference_heatmap_from_error(np.where(mask, error, 0))
    return [(f"{label}: reference", reference), ("White skin + SH", ours),
            (f"dE76 mean {stats['mean']:.1f}, p95 {stats['p95']:.1f}", heat)], stats


def compose(folders, out):
    """Write a sheet and return its per-pose statistics."""
    rows, stats = [], {}
    for folder in folders:
        with Image.open(folder / REFERENCE_PICTURE) as ref, Image.open(folder / "ours.bmp") as ours:
            row, stats[folder.name] = comparison_row(ref.convert("RGB"), ours.convert("RGB"),
                                                     np.load(folder / SKIN_COVERAGE), folder.name)
        rows.append(row)
    save_png(still_sheet(rows), out)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folders", nargs="+", type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    args = parser.parse_args()
    print(json.dumps(compose(args.folders, args.out), indent=2))


if __name__ == "__main__":
    main()
