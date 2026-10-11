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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=pathlib.Path)
    parser.add_argument("document", type=pathlib.Path)
    args = parser.parse_args()
    table = ["| Build id | Kernel | Cycles | Per | Build / vertex | Max byte error | Mean byte error |",
             "|---|---|---:|---|---:|---:|---:|"]
    table.extend("| " + " | ".join(row) + " |" for row in rows(args.capture.read_text(encoding="utf-8")))
    replace_block(args.document, "skin-light-board", "\n".join(table))


if __name__ == "__main__":
    main()
