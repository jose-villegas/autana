#!/usr/bin/env python3
"""Compose host frames and paired skin crops with masked dE76 from pose render folders."""
import argparse
import json
import pathlib
import sys
from itertools import combinations

import numpy as np
from PIL import Image, ImageOps
from scipy.ndimage import binary_erosion

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from docs.figures import save_png
sys.path.insert(0, str(ROOT / "launcher/tools/render"))
from colour_maths import SRGB_TO_XYZ
from render_compare import ZOOM, _linear_rgb, delta_e76, still_sheet
from r3d.skin_reference import MASK_THRESHOLD, PER_SKIN_COVERAGE, REFERENCE_PICTURE, SKIN_COVERAGE

CROP_MARGIN = 8
CROP_LABEL_WIDTH = 136
PROBE_TERMS = ("sun", "bounce")
TERM_LABELS = {"sun": "Sun only", "bounce": "Bounce only", "total": "Total"}
LUMINANCE_WEIGHTS = SRGB_TO_XYZ[1]


def comparison_row(reference, ours, coverage, label, terms=None, tonemap_white=None):
    """Crop both renders to one skin and score the interior of its coverage."""
    mask = coverage > MASK_THRESHOLD
    if mask.shape != (reference.height, reference.width):
        raise ValueError("skin mask and reference sizes differ")
    bounds = Image.fromarray(mask).getbbox()
    if bounds is None:
        stats = {"mean": None, "p95": None, "pixels": 0}
        if terms is not None:
            stats.update(irradiance={term: None for term in TERM_LABELS}, bounce_share=None)
        return [], stats
    left, top, right, bottom = bounds
    box = (max(0, left - CROP_MARGIN), max(0, top - CROP_MARGIN),
           min(reference.width, right + CROP_MARGIN), min(reference.height, bottom + CROP_MARGIN))
    host_crop, ref_crop = ours.crop(box), reference.crop(box)
    interior = binary_erosion(mask)
    if interior.any():
        mask = interior
    left, top, right, bottom = box
    mask = mask[top:bottom, left:right]
    chosen = delta_e76(host_crop, ref_crop)[mask]
    stats = {"mean": float(chosen.mean()), "p95": float(np.percentile(chosen, 95)), "pixels": int(chosen.size)}
    size = (host_crop.width * ZOOM, host_crop.height * ZOOM)
    padding = (0, 0, max(0, CROP_LABEL_WIDTH - size[0]), 0)
    crops = {label: host_crop}
    if terms is not None:
        by_term = {term: picture.crop(box) for term, picture in terms.items()}
        by_term["total"] = host_crop
        irradiance = {}
        for term, crop in by_term.items():
            mapped = _linear_rgb(crop)[mask]
            denominator = 1.0 - tonemap_white * mapped
            if np.any(denominator <= 0):
                raise ValueError("tone-mapped pixels exceed the invertible range")
            linear = mapped / denominator
            irradiance[term] = float((linear @ LUMINANCE_WEIGHTS).mean())
        stats.update(irradiance=irradiance,
                     bounce_share=irradiance["bounce"] / irradiance["total"] if irradiance["total"] else None)
        crops = {f"{label} {TERM_LABELS[term]}": crop for term, crop in by_term.items()}
    crops[f"Ref  dE76 {stats['mean']:.1f}"] = ref_crop
    return [(text, ImageOps.expand(crop.resize(size, Image.Resampling.NEAREST), padding))
            for text, crop in crops.items()], stats


def pixel_diffs(pictures, masks):
    """Count changed pixels against reference coverage and bound them by the skin crop boxes."""
    visible = np.any(np.asarray(masks) > MASK_THRESHOLD, axis=0)
    near_skin = np.zeros_like(visible)
    for mask in masks:
        bounds = Image.fromarray(mask > MASK_THRESHOLD).getbbox()
        if bounds is not None:
            left, top, right, bottom = bounds
            near_skin[max(0, top - CROP_MARGIN):bottom + CROP_MARGIN,
                      max(0, left - CROP_MARGIN):right + CROP_MARGIN] = True
    diffs = {}
    for (first, a), (second, b) in combinations(pictures.items(), 2):
        changed = np.any(np.asarray(a) != np.asarray(b), axis=2)
        diffs[f"{first}_vs_{second}"] = {
            "inside_reference_skins": int((changed & visible).sum()),
            "outside_reference_skins": int((changed & ~visible).sum()),
            "outside_skin_crops": int((changed & ~near_skin).sum()),
        }
    return diffs


def compose(folders, out, *, tonemap_white=None):
    """Write a row per still and return its statistics in skin export order."""
    rows, stats = [], {}
    for folder in folders:
        with Image.open(folder / REFERENCE_PICTURE) as ref, Image.open(folder / "ours.bmp") as ours:
            ref, ours = ref.convert("RGB"), ours.convert("RGB")
        if ref.size != ours.size:
            raise ValueError("host and reference sizes differ")
        mask_file = folder / PER_SKIN_COVERAGE
        masks = np.load(mask_file) if mask_file.is_file() else [np.load(folder / SKIN_COVERAGE)]
        terms = None
        if tonemap_white is not None:
            terms = {}
            for term in PROBE_TERMS:
                with Image.open(folder / f"{term}.bmp") as picture:
                    if picture.size != ours.size:
                        raise ValueError("lighting term and total sizes differ")
                    terms[term] = picture.convert("RGB")
        row = [(f"{folder.name}: total probe lighting", ours)]
        skins = []
        for index, coverage in enumerate(masks, 1):
            label = f"Skin {index}:" if terms is not None else f"Skin {index}: host"
            panels, skin_stats = comparison_row(ref, ours, coverage, label, terms, tonemap_white)
            row.extend(panels)
            skins.append({"skin": index, **skin_stats})
        stats[folder.name] = skins
        if terms is not None:
            diffs = pixel_diffs({"total": ours, **terms}, masks)
            if any(counts["outside_skin_crops"] for counts in diffs.values()):
                raise ValueError(f"{folder.name} has changes outside the skin crops: {diffs}")
            stats[folder.name] = {"frame": json.loads((folder / "pose.json").read_text())[0]["frame"],
                                  "skins": skins, "pixel_diffs": diffs}
        rows.append(row)
    save_png(still_sheet(rows), out)
    return stats


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folders", nargs="+", type=pathlib.Path)
    parser.add_argument("--out", required=True, type=pathlib.Path)
    parser.add_argument("--tonemap-white", type=float, help="include sun and bounce renders with this tone-map setting")
    args = parser.parse_args()
    print(json.dumps(compose(args.folders, args.out, tonemap_white=args.tonemap_white), indent=2))


if __name__ == "__main__":
    main()
