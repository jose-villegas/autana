#!/usr/bin/env python3
"""Compose host frames and paired skin crops with masked dE76 from pose render folders."""
import argparse
import json
import pathlib
import sys

import numpy as np
from PIL import Image, ImageOps
from scipy.ndimage import binary_erosion

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figures import save_png
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from render_compare import ZOOM, delta_e76, still_sheet
from r3d.skin_reference import MASK_THRESHOLD, PER_SKIN_COVERAGE, REFERENCE_PICTURE, SKIN_COVERAGE

CROP_MARGIN = 8
CROP_LABEL_WIDTH = 136


def comparison_row(reference, ours, coverage, label):
    """Crop both renders to one skin and score the interior of its coverage."""
    mask = coverage > MASK_THRESHOLD
    if mask.shape != (reference.height, reference.width):
        raise ValueError("skin mask and reference sizes differ")
    bounds = Image.fromarray(mask).getbbox()
    if bounds is None:
        return [], {"mean": None, "p95": None, "pixels": 0}
    left, top, right, bottom = bounds
    box = (max(0, left - CROP_MARGIN), max(0, top - CROP_MARGIN),
           min(reference.width, right + CROP_MARGIN), min(reference.height, bottom + CROP_MARGIN))
    host_crop, ref_crop = ours.crop(box), reference.crop(box)
    interior = binary_erosion(mask)
    if interior.any():
        mask = interior
    left, top, right, bottom = box
    chosen = delta_e76(host_crop, ref_crop)[mask[top:bottom, left:right]]
    stats = {"mean": float(chosen.mean()), "p95": float(np.percentile(chosen, 95)), "pixels": int(chosen.size)}
    size = (host_crop.width * ZOOM, host_crop.height * ZOOM)
    padding = (0, 0, max(0, CROP_LABEL_WIDTH - size[0]), 0)
    labels = (label, f"Ref  dE76 {stats['mean']:.1f}")
    return [(text, ImageOps.expand(crop.resize(size, Image.Resampling.NEAREST), padding))
            for text, crop in zip(labels, (host_crop, ref_crop))], stats


def compose(folders, out):
    """Write a row per still and return its statistics in skin export order."""
    rows, stats = [], {}
    for folder in folders:
        with Image.open(folder / REFERENCE_PICTURE) as ref, Image.open(folder / "ours.bmp") as ours:
            ref, ours = ref.convert("RGB"), ours.convert("RGB")
        if ref.size != ours.size:
            raise ValueError("host and reference sizes differ")
        mask_file = folder / PER_SKIN_COVERAGE
        masks = np.load(mask_file) if mask_file.is_file() else [np.load(folder / SKIN_COVERAGE)]
        row = [(f"{folder.name}: probe lighting", ours)]
        stats[folder.name] = []
        for index, coverage in enumerate(masks, 1):
            panels, skin_stats = comparison_row(ref, ours, coverage, f"Skin {index}: host")
            row.extend(panels)
            stats[folder.name].append({"skin": index, **skin_stats})
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
