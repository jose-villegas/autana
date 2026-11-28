"""Join measured revisions into the scorecard, full part CSVs and comparison sheets."""
from collections import defaultdict
import csv
import importlib
import os
import pathlib
import sys

import numpy as np
from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path[:0] = [str(TOOLS), str(TOOLS / "perf"), str(TOOLS / "render")]

from frame_cost import US_PER_MS
from render_compare import still_sheet

UPSCALE = 2
OVERLAY_COLOUR = (255, 64, 32)
OVERLAY_ALPHA = 0.65
NOT_MEASURED = "not measured"
NOT_LOGGED = "not logged"
REPORT_NAME = "scorecard.md"


def nearest_upscale(picture, factor=UPSCALE):
    return picture.resize((picture.width * factor, picture.height * factor), Image.Resampling.NEAREST)


def tint_overlay(picture, mask, colour=OVERLAY_COLOUR, alpha=OVERLAY_ALPHA):
    weight = np.asarray(mask, dtype=float)
    if weight.shape != (picture.height, picture.width) or not np.all((0 <= weight) & (weight <= 1)):
        raise ValueError(f"overlay must provide a {picture.height}×{picture.width} mask with values in [0, 1]")
    weight = weight[..., None] * alpha
    rgb = np.asarray(picture.convert("RGB"))
    return Image.fromarray(np.rint(rgb * (1 - weight) + np.asarray(colour) * weight).astype(np.uint8))


def markdown_row(*cells):
    return "| " + " | ".join(str(cell).replace("|", r"\|").replace("\n", " ") for cell in cells) + " |"


def timing_reading(row):
    """A capture's playback timings and the limits logged with them."""
    time = f'{row["mean"] / US_PER_MS:.3f} / {row["p95"] / US_PER_MS:.3f} ms ({row["label"]})'
    limits = [f"{label} {row[field] / US_PER_MS:.3f} ms"
              for label, field in (("mean goal", "goal"), ("p95 ceiling", "ceiling")) if row.get(field) is not None]
    return time + "<br>" + ", ".join(limits) if limits else time


def timing_tables(arms):
    ceilings = [{(r["row"], r["quarter"]): r for r in arm.get("ceilings", [])} for arm in arms]
    captures = [defaultdict(list) for arm in arms]
    for arm, grouped in zip(arms, captures, strict=True):
        for row in arm.get("captures", []):
            grouped[row["row"], row["quarter"]].append(row)
    for key in dict.fromkeys(key for arm in (*ceilings, *captures) for key in arm):
        label = " · ".join(key)
        time_cells, heap_cells = [], []
        for budget, recorded in zip(ceilings, captures, strict=True):
            readings = recorded.get(key, [])
            time = "<br>".join(map(timing_reading, readings)) or NOT_MEASURED
            if key in budget:
                limit = budget[key]
                time += (f'<br>mean goal {limit["mean"] / US_PER_MS:.3f} ms'
                         f', p95 ceiling {limit["p95"] / US_PER_MS:.3f} ms')
            heap = "<br>".join((f'{r["heap"]["free"]:,} / {r["heap"]["largest"]:,} B'
                                if r["heap"] else NOT_LOGGED) + f' ({r["label"]})' for r in readings)
            time_cells.append(time)
            heap_cells.append(heap or NOT_MEASURED)
        yield markdown_row(f"A308 mean / p95 · {label}", *time_cells)
        yield markdown_row(f"App heap after the first frame · free / largest · {label}", *heap_cells)


def metric(arm, name):
    value = arm.get(name)
    if value is None:
        return NOT_MEASURED
    if isinstance(value, dict):
        if "reason" in value:
            return f'{NOT_MEASURED} ({value["reason"]})'
        if name == "held_out":
            return f'{value["mean"]:.3f} ({", ".join(value["sets"])} · {value["views"]:,} views)'
        reading = f'{value["pixels"]:,}' if "pixels" in value else f'{value["holed"]}/{value["frames"]}'
        return reading if value.get("ceiling") is None else f'{reading} (ceiling {value["ceiling"]:,})'
    return f"{value:,}"


def grouped_parts(parts):
    groups = {}
    for row in parts:
        group = groups.setdefault(row["material"], dict(iou=1.0, hausdorff=None, missing=0))
        group["iou"] = min(group["iou"], row["iou"])
        if row["hausdorff"] is None:
            group["missing"] += 1
        else:
            group["hausdorff"] = max(group["hausdorff"] or 0.0, row["hausdorff"])
    return groups


def part_classes(arms):
    parent, tip = [grouped_parts(arm.get("parts", [])) for arm in arms]

    def worst(name):
        group = tip.get(name, parent.get(name))
        return group["iou"], -(group["hausdorff"] or 0.0), name

    def cell(group):
        if group is None:
            return NOT_MEASURED
        distance = "—" if group["hausdorff"] is None else f'{group["hausdorff"]:.5f}'
        missing = f' ({group["missing"]} undefined)' if group["missing"] else ""
        return f'{group["iou"]:.4f} / {distance}{missing}'

    return [markdown_row(name, cell(parent.get(name)), cell(tip.get(name)))
            for name in sorted(parent.keys() | tip.keys(), key=worst)]


def markdown(parent, tip):
    lines = ["# Track scorecard", "", f'Parent `{parent["revision"]}` · Tip `{tip["revision"]}`']
    labels = (("See-through pixels", "see_through"), ("Sky holed frames", "sky"),
              ("Held-out mean dE76", "held_out"), ("Pack bytes", "pack_bytes"))
    for key in dict.fromkeys((*parent["scenes"], *tip["scenes"])):
        arms = [rev["scenes"].get(key, {}) for rev in (parent, tip)]
        lines.extend(("", f'## {key.title()}', "", "| Metric | Parent | Tip |", "|---|---|---|"))
        lines.extend(timing_tables(arms))
        lines.extend(markdown_row(label, *(metric(arm, field) for arm in arms)) for label, field in labels)
        classes = part_classes(arms)
        if classes:
            lines.extend(("", "| Part class · min front IoU / max relative Hausdorff | Parent | Tip |", "|---|---|---|",
                          *classes, "", f"[All parts]({key}-parts.csv)"))
        if all(arm.get("images") for arm in arms):
            lines.extend(("", f"![{key.title()} camera path comparison]({key}.png)"))
    return "\n".join(lines) + "\n"


def write_parts(path, arms):
    fields = ("material", "source_object", "part", "iou", "hausdorff")
    keys = fields[:3]
    joined = {}
    for arm, revision in zip(arms, ("parent", "tip")):
        for part in arm.get("parts", []):
            key = tuple(part[field] for field in keys)
            row = joined.setdefault(key, {field: part[field] for field in keys})
            row.update({f"{revision}_{field}": part[field] for field in fields[3:]})
    columns = (*keys, "parent_iou", "parent_hausdorff", "tip_iou", "tip_hausdorff")
    with path.open("w", encoding="utf-8", newline="") as out:
        writer = csv.DictWriter(out, columns)
        writer.writeheader()
        writer.writerows(joined[key] for key in sorted(joined))


def overlay_callable(spec):
    module, separator, name = spec.partition(":")
    if not separator or not module or not name:
        raise ValueError("--overlay needs MODULE:FUNCTION")
    if os.getcwd() not in sys.path:
        sys.path.insert(0, os.getcwd())
    function = getattr(importlib.import_module(module), name)
    if not callable(function):
        raise ValueError(f"--overlay {spec!r} must name a callable")
    return function


def write_sheet(path, arms, roots, overlay, tip_tree):
    before, after = arms
    pairs = list(zip(before["images"], after["images"], strict=True))
    masks = [None] * len(pairs)
    if overlay:
        masks = list(overlay(tip_tree / after["path"], tip_tree, after["poses"], tuple(after["size"])))
    rows = []
    for (left, right), mask in zip(pairs, masks, strict=True):
        with Image.open(roots[0] / left) as a, Image.open(roots[1] / right) as b:
            panels = [("Parent", nearest_upscale(a)), ("Tip", nearest_upscale(b))]
            if overlay:
                panels.append(("Tip overlay", nearest_upscale(tint_overlay(b, mask))))
            rows.append(panels)
    still_sheet(rows).save(path)


def write_report(out, revisions, roots, overlay=None, tip_tree=None):
    parent, tip = revisions
    out.mkdir(parents=True, exist_ok=True)
    for key in dict.fromkeys((*parent["scenes"], *tip["scenes"])):
        arms = [rev["scenes"].get(key, {}) for rev in revisions]
        if any(arm.get("parts") for arm in arms):
            write_parts(out / f"{key}-parts.csv", arms)
        if all(arm.get("images") for arm in arms):
            write_sheet(out / f"{key}.png", arms, roots, overlay, tip_tree)
    (out / REPORT_NAME).write_text(markdown(parent, tip), encoding="utf-8")
    return out / REPORT_NAME
