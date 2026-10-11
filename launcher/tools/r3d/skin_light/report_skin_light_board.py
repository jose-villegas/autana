"""Generate the board table from committed suite result lines without measuring the host."""
import argparse
import pathlib
import re
import statistics
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[2] / "render"))
from generated_blocks import replace_block


def samples(text):
    build = label = ""
    vertices = 0
    pending = None
    for line in text.splitlines():
        if line.startswith("BUILD_ID "):
            if pending:
                yield pending
                pending = None
            build = line.split()[1]
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
    builds = [row[0] for row in measured if row[1] == "fused_table_1"]
    table = ["| Mode | Lights | Cycles / vertex | Worst byte error | Mean byte error | Table build cycles | Renderer internal bytes | Scene internal bytes |",
             "|---|---:|---:|---:|---:|---:|---:|---:|"]
    if not builds:
        return "Board capture for the three runtime modes is pending.\n\n" + "\n".join(table)
    build = builds[-1]
    costs = {row[1]: row for row in measured if row[0] == build}
    memory = {}
    current_build = ""
    for line in text.splitlines():
        if line.startswith("BUILD_ID "):
            current_build = line.split()[1]
        match = re.search(r"RAM mode=(\w+) renderer_bytes=(\d+) scene_bytes=(\d+)", line)
        if match and current_build == build:
            memory[match[1]] = (match[2], match[3])
    for mode in ("direct", "renormalised", "table"):
        for lights in (1, 2, 4):
            row = costs.get(f"fused_{mode}_{lights}")
            if row is None:
                continue
            build_row = costs.get(f"table_build_{lights}") if mode == "table" else None
            values = [mode, str(lights), row[2], row[-2], row[-1], build_row[2] if build_row else "—",
                      *memory.get(mode, ("—", "—"))]
            table.append("| " + " | ".join(values) + " |")
    return f"Build `{build}`.\n\n" + "\n".join(table)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("document", type=pathlib.Path)
    args = parser.parse_args()
    table = ["| Build id | Kernel | Cycles | Per | Build / vertex | Max byte error | Mean byte error |",
             "|---|---|---:|---|---:|---:|---:|"]
    table.extend("| " + " | ".join(row) + " |" for row in rows(args.capture.read_text(encoding="utf-8")))
    replace_block(args.document, "skin-light-board", "\n".join(table))
    replace_block(args.document, "skin-light-modes", lighting_modes(args.capture.read_text(encoding="utf-8")))


if __name__ == "__main__":
    main()
