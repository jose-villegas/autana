"""Generate the board table from committed suite result lines without measuring the host."""
import argparse
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "render"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "perf"))
from capture import build_ids
from generated_blocks import replace_block


def samples(text):
    build = label = ""
    vertices = 0
    pending = None
    for line in text.splitlines():
        if build_ids(line):
            if pending:
                yield pending
                pending = None
            build = build_ids(line)[0]
        match = re.search(r"vertices=(\d+)", line)
        if match:
            vertices = int(match[1])
        match = re.search(r"skin_light_perf: (test_\w+) .*med=\d+cycles", line)
        if match:
            if pending:
                yield pending
                pending = None
            label = match[1].removeprefix("test_")
        match = re.search(r"COST cycles_per_(vertex|build)=([\d.]+)", line)
        if match:
            cost = float(match[2])
            pending = [build, label, f"{cost:.3f}", match[1],
                       f"{cost / vertices:.3f}" if match[1] == "build" else "—", "—", "—"]
        match = re.search(r"QUALITY max=(\d+) mean=([\d.]+)", line)
        if match and pending:
            pending[-2:] = match.groups()
    if pending:
        yield pending


def rows(text):
    grouped = {}
    for row in samples(text):
        grouped.setdefault((row[0], row[1], row[3]), []).append(row)
    for runs in grouped.values():
        row = runs[0].copy()
        for column in (2, 4):
            if row[column] != "—":
                row[column] = f"{statistics.median(float(run[column]) for run in runs):.3f}"
        quality = [run for run in runs if run[-2] != "—"]
        if quality:
            row[-2] = str(max(int(run[-2]) for run in quality))
            row[-1] = max(quality, key=lambda run: float(run[-1]))[-1]
        yield row


def lighting_modes(text):
    measured = list(rows(text))
    builds = [row[0] for row in measured if re.fullmatch(r"fused_table_\d+", row[1])]
    table = ["| Mode | Lights | Cycles / vertex | Worst byte error | Mean byte error | Table build cycles / light change | Normal-palette build cycles / frame | Renderer internal bytes | Scene internal bytes | Scene PSRAM bytes |",
             "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    if not builds:
        raise ValueError("no runtime table capture")
    build = builds[-1]
    costs = {row[1]: row for row in measured if row[0] == build}
    memory = {}
    current_build = ""
    for line in text.splitlines():
        if build_ids(line):
            current_build = build_ids(line)[0]
        match = re.search(r"RAM mode=(\w+) renderer_bytes=(\d+) scene_bytes=(\d+).*scene_metadata_psram=(\d+)", line)
        if match and current_build == build:
            memory[match[1]] = (match[2], match[3], match[4])
    if not any(re.fullmatch(r"table_build_\d+", name) for name in costs):
        raise ValueError("no runtime table build capture")
    palette_row = costs.get("normal_palette_build")
    for mode in ("direct", "renormalised", "table"):
        light_counts = sorted(int(match[1]) for name in costs
                              if (match := re.fullmatch(rf"fused_{mode}_(\d+)", name)))
        for lights in light_counts:
            row = costs.get(f"fused_{mode}_{lights}")
            if row is None:
                continue
            build_row = costs.get(f"table_build_{lights}") if mode == "table" else None
            values = [mode, str(lights), row[2], row[-2], row[-1], build_row[2] if build_row else "—",
                      palette_row[2] if palette_row else "—",
                      *memory.get(mode, ("—", "—", "—"))]
            table.append("| " + " | ".join(values) + " |")
    return f"Build `{build}`.\n\n" + "\n".join(table)


def cost_chart(text):
    """A bar chart of the modes' cycles per vertex at each light count, after skinning alone; the last
    captured build wins."""
    measured = {row[1]: row for row in rows(text) if row[3] == "vertex"}
    bars = [("skin only", measured["skin_only"])]
    for mode in ("direct", "renormalised", "table"):
        bars += sorted(((f"{mode} {match[1]}", row) for name, row in measured.items()
                        if (match := re.fullmatch(rf"fused_{mode}_(\d+)", name))),
                       key=lambda bar: int(bar[0].rsplit(" ", 1)[1]))
    labels = ", ".join(f'"{label}"' for label, _ in bars)
    values = ", ".join(f"{float(row[2]):.0f}" for _, row in bars)
    return "\n".join(["```mermaid", "xychart-beta", '    title "Cycles per vertex: mode and light count"',
                      f"    x-axis [{labels}]", '    y-axis "cycles"', f"    bar [{values}]", "```"])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("document", type=pathlib.Path)
    parser.add_argument("--import-document", type=pathlib.Path,
                        default=pathlib.Path(__file__).resolve().parents[4] / "docs/render/Mesh-Import.md")
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    capture = args.capture.read_text(encoding="utf-8")
    try:
        modes = lighting_modes(capture)
    except ValueError as error:
        parser.error(str(error))
    table = ["| Build id | Kernel | Cycles | Per | Build / vertex | Max byte error | Mean byte error |",
             "|---|---|---:|---|---:|---:|---:|"]
    table.extend("| " + " | ".join(row) + " |" for row in rows(capture))
    command = ("python launcher/tools/r3d/skin_light/report_skin_light_board.py "
               "docs/render/data/skin-light-board.txt docs/render/Skinned-Lighting.md --check")
    changed = False
    for document, name, body in ((args.document, "skin-light-board", "\n".join(table)),
                                 (args.document, "skin-light-modes", modes),
                                 (args.document, "skin-light-chart", cost_chart(capture)),
                                 (args.import_document, "skin-light-import", modes)):
        changed |= replace_block(document, name, body, check=args.check, command=command)
    return int(args.check and changed)


if __name__ == "__main__":
    sys.exit(main())
