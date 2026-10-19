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
SMALL_FILL_STYLES = re.findall(r"SMALL_FILL_([A-Z]+)(?=\s*[,}])", re.search(
    r"typedef enum \{([^}]+)\} small_fill_style_t;", VIEW_HEADER.read_text()).group(1) + "}")
SWEEP_GATE_OFF = 64
MICROSECONDS_PER_MILLISECOND = 1000

SCORE = re.compile(r"^(.+): mean DeltaE76 ([\d.]+), p95 DeltaE76 ([\d.]+), luma SSIM (-?[\d.]+), "
                   r"edge DeltaE76 ([\d.]+), interior DeltaE76 ([\d.]+)", re.M)


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


def small_fill_board(data, scene_id, object_name):
    """Flat camera settings plus optional camera-style measurements in p7."""
    mesh = f"run_{scene_id}_perf_suite/{object_name}_fitted_full quarter=landscape"
    tables = []
    for name in ("p5a", "p7"):
        if name == "p7" and not (data / f"{name}-plan.json").exists():
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
            if name == "p5a":
                ceiling = knobs["r3d_span.small_max_side"]
                gate = knobs["r3d_span.small_flat_spread_far"]
                gate = 0 if gate == SWEEP_GATE_OFF else gate
                style = SMALL_FILL_STYLES[0]
            else:
                ceiling = knobs["render_lab.small_fill_ceiling"]
                gate = knobs["render_lab.small_fill_gate"]
                style = SMALL_FILL_STYLES[knobs["render_lab.small_fill_style"]]
            values = []
            for metric in (mesh, skin[0]):
                value = row["metrics"][metric]["mean"]
                values.extend((f"{value / MICROSECONDS_PER_MILLISECOND:.3f}", f"{100 * (value / base[metric]['mean'] - 1):+.2f}%"))
            measured.append((ceiling, gate, style, *values, f"{name}, point {row['point']}"))
        tables.append(markdown(["Ceiling", "Gate", "Style", "Mesh frame ms", "vs first row",
                               "Skinned render ms", "vs first row", "Capture"], measured))
    return "\n".join(tables)


def write_small_fill(work, out, scene_id, object_name, data):
    """Camera axes from the source registry, temporal CSVs and recorded board sweeps."""
    out.mkdir(parents=True, exist_ok=True)
    from ctypes import c_uint8
    header = VIEW_HEADER.read_text()
    defaults = dict(re.findall(r"#define SMALL_FILL_(CEILING|GATE)_DEFAULT\s+(\d+)", header))
    style = re.search(r"#define SMALL_FILL_STYLE_DEFAULT\s+SMALL_FILL_(\w+)", header).group(1)
    catalog = {name.lower(): (int(default), 0, c_uint8(-1).value) for name, default in defaults.items()}
    catalog["style"] = (SMALL_FILL_STYLES.index(style), 0, len(SMALL_FILL_STYLES) - 1)
    effects = {
        "ceiling": "Largest bounding-box side in pixel centres; 0 disables small fill",
        "gate": "Vertex RGB565 spread limit at the ceiling; 0 disables the colour gate",
        "style": ", ".join(f"{name} ({index})" for index, name in enumerate(SMALL_FILL_STYLES)),
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
        ["Look vs Ceiling 4, no gate", "Extra frame-to-frame change (levels of 255)", "Blocks popping (>8/255)"], temporal), encoding="utf-8")
    (out / "small-fill-board.md").write_text(small_fill_board(data, scene_id, object_name), encoding="utf-8")


def write_tables(work, out, scene_id, object_name):
    from PIL import Image
    from render_compare import read_video, reference_video
    from r3d.bake_fidelity import table

    out.mkdir(parents=True, exist_ok=True)
    indirect = scores(work / "indirect-compare.log")
    smooth = indirect["two bounces"]
    full = scores(work / "committed-smooth-compare.log")["frames mean"]
    lite = scores(work / f"{object_name}_lite-compare.log")["frames mean"]
    flat = scores(work / "fidelity-compare.log")["frames mean"]
    (out / f"{scene_id}-fidelity.md").write_text(table([
        ("Full smooth", full), ("Lite smooth", lite), ("Flat, committed", flat)]) + "\n", encoding="utf-8")
    smooth_frames = work / "smooth-reference"
    smooth_frames.mkdir(exist_ok=True)
    _, frames = read_video(work / "committed-smooth.avi")
    for index, frame in enumerate(frames):
        Image.fromarray(frame).save(smooth_frames / f"{index:04d}.png")
    _, pair = reference_video(work / "fidelity-flat.avi", smooth_frames, scale=1)
    (out / f"{scene_id}-flat-smooth.md").write_text(table([
        ("Flat against smooth, fidelity poses", (pair.mean_delta_e, pair.p95_delta_e, pair.ssim_luma,
                                                pair.edge_delta_e, pair.interior_delta_e))]) + "\n", encoding="utf-8")
    (out / f"{scene_id}-flat-sampling.md").write_bytes((work / "sampling/table.md").read_bytes())
    (out / f"{scene_id}-indirect.md").write_text(table([
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
    (out / f"{scene_id}-indirect-look.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("work", type=pathlib.Path)
    parser.add_argument("out", type=pathlib.Path)
    parser.add_argument("scene_id")
    parser.add_argument("object_name")
    parser.add_argument("--small-fill-data", type=pathlib.Path)
    args = parser.parse_args()
    if args.small_fill_data:
        write_small_fill(args.work, args.out, args.scene_id, args.object_name, args.small_fill_data)
    else:
        write_tables(args.work, args.out, args.scene_id, args.object_name)
