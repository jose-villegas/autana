"""Markdown tables for generated doc blocks: write, parse back and collapse
them, plus the CPU fidelity tables from the doc-image run's comparison logs."""
import argparse
import csv
import json
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

VIEW_HEADER = pathlib.Path(__file__).resolve().parents[2] / "main/render/render_view.h"
SWEEP_GATE_OFF = 64
# A fidelity table's score columns after Variant, as r3d/bake_fidelity.py's table() writes them too; the doc
# charts read them back by these names.
SCORE_COLUMNS = ("Mean dE76", "p95 dE76", "Luma SSIM", "Edge dE76", "Interior dE76")
# The indirect-look table's columns after Look; the doc charts read them back by these names.
LOOK_COLUMNS = ("Mean dE76, physical", "Mean dE76, own", "p95, physical", "SSIM, physical")
MICROSECONDS_PER_MILLISECOND = 1000

SCORE = re.compile(r"^(.+): mean DeltaE76 ([\d.]+), p95 DeltaE76 ([\d.]+), luma SSIM (-?[\d.]+), "
                   r"edge DeltaE76 ([\d.]+), interior DeltaE76 ([\d.]+)", re.M)


def small_fill_styles():
    return re.findall(r"SMALL_FILL_([A-Z]+)(?=\s*[,}])", re.search(
        r"typedef enum \{([^}]+)\} small_fill_style_t;", VIEW_HEADER.read_text()).group(1) + "}")


def markdown(headers, rows):
    cell = lambda value: str(value).replace("|", "\\|").replace("\n", " ")
    headers = [cell(value) for value in headers]
    lines = ["| " + " | ".join(headers) + " |", "|" + "|".join("---" for _ in headers) + "|"]
    return "\n".join(lines + ["| " + " | ".join(map(cell, row)) + " |" for row in rows]) + "\n"


def table_rows(text, include_empty=False):
    """Yield line number, headers and cells; include_empty also yields header-only tables with no cells."""
    lines = text.splitlines()
    number = 0
    cells = lambda row: [cell.strip().replace('\\|', '|') for cell in re.split(r'(?<!\\)\|', row.strip())[1:-1]]
    while number + 1 < len(lines):
        header, divider = lines[number].strip(), lines[number + 1].strip()
        if not (header.startswith("|") and divider.startswith("|") and
                re.fullmatch(r"[| :\-]+", divider)):
            number += 1
            continue
        headings = cells(header)
        number += 2
        if include_empty and (number == len(lines) or not lines[number].lstrip().startswith("|")):
            yield number, headings, []
        while number < len(lines) and lines[number].lstrip().startswith("|"):
            yield number + 1, headings, cells(lines[number])
            number += 1



def parse_table(body, key, required=()):
    tables = []
    previous = None
    for line, headers, cells in table_rows(body, include_empty=True):
        if key not in headers:
            continue
        if headers is not previous:
            missing = set(required) - set(headers)
            if missing:
                raise ValueError(f'{key}: missing columns: {", ".join(sorted(missing))}')
            tables.append((headers, []))
            previous = headers
        if not cells:
            continue
        if len(cells) != len(headers):
            raise ValueError(f'{key}: malformed table row: {body.splitlines()[line - 1]}')
        tables[-1][1].append(dict(zip(headers, cells)))
    if len(tables) != 1:
        raise ValueError(f'{key}: expected one table, found {len(tables)}')
    return tables[0]


def collapsed(summary, table):
    return f'<details><summary>{summary}</summary>\n\n{table}\n</details>\n'


def scores(path):
    found = {label: tuple(float(value) for value in values) for label, *values in
             SCORE.findall(path.read_text(encoding="utf-8"))}
    if not found:
        raise ValueError(f"no fidelity scores in {path}")
    return found


def sweep_rows(data, name):
    """Validated perf_sweep rows and their recorded plan."""
    plan = json.loads((data / f"{name}-plan.json").read_text())
    rows = json.loads((data / f"{name}-sweep.json").read_text())["rows"]
    if len(rows) != len(plan["points"]) or len({row["build"] for row in rows}) != 1:
        raise ValueError(f"{name}: needs one build and every planned point")
    if len({capture["board"] for capture in plan["captures"]}) != 1:
        raise ValueError(f"{name}: needs one board")
    rows = sorted(rows, key=lambda row: row["point"])
    for index, row in enumerate(rows):
        point = plan["points"][index]
        if row["point"] != index or row["knobs"] != point.get("knobs", point):
            raise ValueError(f"{name}: sweep differs from its plan")
        for metric in row["metrics"].values():
            if not metric["observations"] or metric["mean"] <= 0:
                raise ValueError(f"{name}: missing positive measurements")
    return plan, rows


SWEEP_SUMMARIES = {"ceiling-gate": "Ceiling by gate, FLAT, rows in the sweep's point order",
                   "styles": "Camera style, rows in the sweep's point order"}


def small_fill_board(data, scene_id, object_name):
    """Flat camera settings plus optional camera-style measurements, each a collapsed sweep."""
    styles = small_fill_styles()
    mesh = f"run_{scene_id}_perf_suite/{object_name}_fitted_full quarter=landscape"
    tables = []
    for name in ("ceiling-gate", "styles"):
        if name == "styles" and not (data / f"{name}-plan.json").exists():
            continue
        _, rows = sweep_rows(data, name)
        skin = [key for key in rows[0]["metrics"] if re.search(
            r"/render paused=0 (?:joints=\d+ )?quarter=landscape$", key)]
        if len(skin) != 1:
            raise ValueError(f"{name}: needs one skinned landscape render metric")
        base = rows[0]["metrics"]
        measured = []
        for row in rows:
            knobs = row["knobs"]
            if name == "ceiling-gate":
                ceiling = knobs["r3d_span.small_max_side"]
                gate = knobs["r3d_span.small_flat_spread_far"]
                gate = 0 if gate == SWEEP_GATE_OFF else gate
                style = styles[0]
            else:
                ceiling = knobs["render_lab.small_fill_ceiling"]
                gate = knobs["render_lab.small_fill_gate"]
                style = styles[knobs["render_lab.small_fill_style"]]
            values = []
            for metric in (mesh, skin[0]):
                value = row["metrics"][metric]["mean"]
                values.extend((f"{value / MICROSECONDS_PER_MILLISECOND:.3f}", f"{100 * (value / base[metric]['mean'] - 1):+.2f}%"))
            measured.append((ceiling, gate, style, *values))
        tables.append(collapsed(SWEEP_SUMMARIES[name], markdown(
            ["Ceiling", "Gate", "Style", "Mesh frame ms", "vs first row", "Skinned render ms", "vs first row"],
            measured)))
    return "\n".join(tables)


def write_small_fill(work, out, scene_id, object_name, data):
    """Camera axes from render_view.h, temporal CSVs and recorded board sweeps."""
    out.mkdir(parents=True, exist_ok=True)
    from ctypes import c_uint8
    styles = small_fill_styles()
    header = VIEW_HEADER.read_text()
    defaults = dict(re.findall(r"#define SMALL_FILL_(CEILING|GATE)_SMOOTH\s+(\d+)", header))
    style = re.search(r"#define SMALL_FILL_SMOOTH[\s\\]*\(\(small_fill_t\)\{[^}]+SMALL_FILL_(\w+)\}\)", header).group(1)
    maxima = dict(re.findall(r"#define SMALL_FILL_(CEILING|GATE)_MAX\s+(\d+)", header))
    catalog = {name.lower(): (int(default), 0, int(maxima.get(name, c_uint8(-1).value)))
               for name, default in defaults.items()}
    catalog["style"] = (styles.index(style), 0, len(styles) - 1)
    effects = {
        "ceiling": "Largest bounding-box side in pixel centres; 0 disables small fill",
        "gate": "Vertex RGB565 spread limit at the ceiling; 0 disables the colour gate",
        "style": ", ".join(f"{name} ({index})" for index, name in enumerate(styles)),
    }
    if set(catalog) != set(effects):
        raise ValueError("small fill needs the camera's three axes")
    levers = [(f"`{name}`", f"{catalog[name][1]}..{catalog[name][2]}", catalog[name][0], effect)
              for name, effect in effects.items()]
    (out / "small-fill-levers.md").write_text(markdown(
        ["Camera field", "Range", "Default", "Effect"], levers), encoding="utf-8")
    temporal = []
    for line in (work / "small-fill-looks.tsv").read_text().splitlines():
        name, label = line.split("|")[:2]
        with (work / f"small-fill-{name}.frames.csv").open() as handle:
            frames = list(csv.DictReader(handle))[1:]
        if not frames:
            raise ValueError("temporal table needs frame transitions")
        temporal.append((label, f'{sum(float(f["extra_change_levels"]) for f in frames) / len(frames):.3f}',
                         f'{sum(float(f["blocks_popping_pct"]) for f in frames) / len(frames):.4f}%'))
    (out / "small-fill-temporal.md").write_text(markdown(
        ["Look vs SMOOTH", "Extra frame-to-frame change (levels of 255)", "Blocks popping (>8/255)"], temporal), encoding="utf-8")
    (out / "small-fill-board.md").write_text(small_fill_board(data, scene_id, object_name), encoding="utf-8")


def variants(scene_path, object_name):
    """(object, fitted) of every renderer the scene places for `object_name`: the object itself and each
    `<object_name>_<variant>`. A variant is fitted when its recipe has a fit."""
    from r3d.import_settings import load_scene
    return [(job.object.name, job.renderer.fit is not None) for job in load_scene(scene_path).renderers
            if job.object.name == object_name or job.object.name.startswith(object_name + "_")]


def variant_label(name, object_name):
    """`atrium_fitted_full` of `atrium` reads "fitted full"; the object itself is "full"."""
    return name[len(object_name) + 1:].replace("_", " ") or "full"


def variant_slug(name, object_name):
    """The variant's part of its image names: "fitted-full"."""
    return variant_label(name, object_name).replace(" ", "-")


# What each variant of a demo object is, keyed by its label, and the variants its pair sheets set it beside, each
# with what changes between them. Every variant the scene places needs an entry here.
VARIANT_NOTES = {
    "full": ("Smooth: one colour per vertex, lit and interpolated", ()),
    "lite": ("The same bake simplified to a smaller budget",
             (("full", "Fewer triangles: small shapes merge or drop and edges step; surfaces keep their colour"),)),
    "flat": ("The full mesh's triangles, one colour per face, no gradients",
             (("full", "One colour per face: a curtain's fold reads as bands"),)),
    "fitted": ("Lite's budget spent on what the flythrough draws, its vertices and colours fitted to the reference",
               (("lite", "Same budget, moved to what the flythrough draws and fitted: arches, shadow edges and banner "
                 "colours come back"),
                ("fitted full", "The same fit recipe on fewer triangles: small shapes merge or drop and edges step"))),
    "fitted full": ("The same recipe at full's budget", (("full", "The fitted recipe on full's finer geometry"),)),
    "flat fitted": ("Flat's budget, one colour per face, its vertices and face colours fitted together",
                    (("flat", "Face colours and positions fitted: banners, arches and floor take their colours back"),)),
}


# Each fidelity sheet's image name and its variants by label: a bake beside its fit with the same shading,
# so the sheet shows what the fit alone buys.
FIDELITY_SHEETS = {
    "bake-fidelity-sheet": ("flat", "flat fitted"),
    "bake-fidelity-full-sheet": ("full", "fitted full"),
}


def fidelity_sheet(scene_path, object_name, image):
    """(object, label) of each variant of FIDELITY_SHEETS[image] the scene places, in its order."""
    by_label = {variant_label(name, object_name): name for name, _ in variants(scene_path, object_name)}
    missing = [label for label in FIDELITY_SHEETS[image] if label not in by_label]
    if missing:
        raise ValueError(f"{scene_path}: no {', '.join(missing)} variant of {object_name} for {image}")
    return [(by_label[label], label) for label in FIDELITY_SHEETS[image]]


def variant_note(name, object_name):
    label = variant_label(name, object_name)
    if label not in VARIANT_NOTES:
        raise ValueError(f"{name}: no entry in doc_tables.VARIANT_NOTES")
    return VARIANT_NOTES[label]


def variant_pairs(names, object_name):
    """(base, variant, what changes) of each pair sheet, in the order of `names`."""
    by_label = {variant_label(name, object_name): name for name in names}
    return [(by_label[base], name, change) for name in names for base, change in variant_note(name, object_name)[1]]


def ranked(found, score):
    """`found`'s (object, fitted) pairs best first: fitted before unfitted, then by mean ΔE."""
    return sorted(found, key=lambda item: (not item[1], score(item[0])[0]))


def score_table(rows):
    """A fidelity table of (label, scores) rows in the order given."""
    lines = ["| Variant | " + " | ".join(SCORE_COLUMNS) + " |", "|---|" + "---:|" * len(SCORE_COLUMNS)]
    lines += [f"| {label} | {v[0]:.3f} | {v[1]:.3f} | {v[2]:.4f} | {v[3]:.3f} | {v[4]:.3f} |" for label, v in rows]
    return "\n".join(lines) + "\n"


def write_tables(work, out, scene_id, object_name, found):
    """The CPU fidelity tables of `found`, the (object, fitted) variants `variants` lists."""
    from PIL import Image
    from render_compare import read_video, reference_video
    from r3d.bake_fidelity import table

    out.mkdir(parents=True, exist_ok=True)
    indirect = scores(work / "indirect-compare.log")
    smooth = indirect["two bounces"]
    score = lambda name: scores(work / f"variant-{name}-compare.log")["frames mean"]  # noqa: E731
    order = ranked(found, score)
    label = lambda name: variant_label(name, object_name)  # noqa: E731
    (out / f"{scene_id}-fidelity.md").write_text(score_table([(label(name), score(name)) for name, _ in order]),
                                                 encoding="utf-8")
    slug = lambda name: variant_slug(name, object_name)  # noqa: E731
    flythrough = [(f"![{label(name)} flythrough](../images/render/{scene_id}-{slug(name)}.gif)",
                   f"**{label(name)}**", variant_note(name, object_name)[0], f"`{scene_id}.{name}`",
                   "yes" if fitted else "no") for name, fitted in order]
    (out / f"{scene_id}-variants.md").write_text(
        markdown(["Flythrough", "Variant", "What it is", "Mesh entry", "Fitted"], flythrough), encoding="utf-8")
    pairs = variant_pairs([name for name, _ in order], object_name)
    sheets = "".join(f"![{label(base)} against {label(name)}](../images/render/compare-{slug(base)}-{slug(name)}.png)\n"
                     f"![{label(base)} against {label(name)}, the places they differ most]"
                     f"(../images/render/compare-{slug(base)}-{slug(name)}.crops.png)\n" for base, name, _ in pairs)
    (out / f"{scene_id}-variant-pairs.md").write_text(markdown(
        ["Pair", "What changes"], [(f"{label(base)} against {label(name)}", change) for base, name, change in pairs])
        + "\n" + sheets, encoding="utf-8")
    smooth_frames = work / "smooth-reference"
    smooth_frames.mkdir(exist_ok=True)
    _, frames = read_video(work / "committed-smooth.avi")
    for index, frame in enumerate(frames):
        Image.fromarray(frame).save(smooth_frames / f"{index:04d}.png")
    _, pair = reference_video(work / f"variant-{object_name}_flat.avi", smooth_frames, scale=1)
    (out / f"{scene_id}-flat-smooth.md").write_text(table([
        ("Flat against smooth, fidelity poses", (pair.mean_delta_e, pair.p95_delta_e, pair.ssim_luma,
                                                pair.edge_delta_e, pair.interior_delta_e))]) + "\n", encoding="utf-8")
    (out / f"{scene_id}-flat-sampling.md").write_bytes((work / "sampling/table.md").read_bytes())
    # The meshes the light study re-bakes, in the fidelity table's order.
    light = {object_name: (indirect["direct light only"], smooth)}
    for mesh in ("lite", "flat"):
        light[f"{object_name}_{mesh}"] = tuple(scores(work / f"{kind}-{mesh}-compare.log")["frames mean"]
                                              for kind in ("direct", "smooth"))
    (out / f"{scene_id}-indirect.md").write_text(score_table(
        [(f"{label(name)}, {kind} light", light[name][index])
         for name, _ in order if name in light for index, kind in enumerate(("direct", "indirect"))]),
        encoding="utf-8")
    look = scores(work / "indirect-look.log")
    lines = ["| Look | " + " | ".join(LOOK_COLUMNS) + " |", "|---|" + "---:|" * len(LOOK_COLUMNS)]
    direct = indirect["direct light only"]
    lines.append(f"| Direct light only | {direct[0]:.3f} | | {direct[1]:.3f} | {direct[2]:.4f} |")
    for label in ("intensity 1", "intensity 2", "intensity 3", "albedo boost 2"):
        physical, own = look[label], look[label + " (own reference)"]
        lines.append(f"| {label} | {physical[0]:.3f} | {own[0]:.3f} | {physical[1]:.3f} | {physical[2]:.4f} |")
    (out / f"{scene_id}-indirect-look.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    # The shell reads these lines; a Windows Python would end them in CR LF.
    sys.stdout.reconfigure(newline="\n")
    if sys.argv[1:2] == ["--variants"]:
        # doc_images_demo.sh renders and scores each of these before the tables.
        print(" ".join(name for name, _ in variants(pathlib.Path(sys.argv[2]), sys.argv[3])))
        sys.exit(0)
    if sys.argv[1:2] == ["--fidelity-sheets"]:
        print(" ".join(FIDELITY_SHEETS))
        sys.exit(0)
    if sys.argv[1:2] == ["--fidelity-sheet"]:
        # One "object label" line per bake of that sheet.
        for name, label in fidelity_sheet(pathlib.Path(sys.argv[2]), sys.argv[3], sys.argv[4]):
            print(name, label)
        sys.exit(0)
    if sys.argv[1:2] == ["--pairs"]:
        # One "base variant base-slug variant-slug" line per pair sheet doc_images_demo.sh draws.
        names = [name for name, _ in variants(pathlib.Path(sys.argv[2]), sys.argv[3])]
        for base, name, _ in variant_pairs(names, sys.argv[3]):
            print(base, name, variant_slug(base, sys.argv[3]), variant_slug(name, sys.argv[3]))
        sys.exit(0)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("scene_id")
    parser.add_argument("object_name")
    parser.add_argument("--small-fill-data", type=pathlib.Path)
    parser.add_argument("--scene", type=pathlib.Path, help="the scene file whose variants the fidelity tables rank")
    args = parser.parse_args()
    if args.small_fill_data:
        write_small_fill(args.work, args.out, args.scene_id, args.object_name, args.small_fill_data)
    else:
        write_tables(args.work, args.out, args.scene_id, args.object_name, variants(args.scene, args.object_name))
