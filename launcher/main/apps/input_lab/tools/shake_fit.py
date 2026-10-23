"""Summarize sample-rate shake candidates from a retained round."""
import argparse
import gzip
from pathlib import Path
import re
import sys
from statistics import median, quantiles

sys.path.insert(0, str(Path(__file__).resolve().parents[4] / "tools"))
from build.c_header import read_define
from render.generated_blocks import replace_block
from render.doc_tables import markdown

HEADER = Path(__file__).resolve().parents[1] / "shake_probe.h"
STEP_MS = read_define(HEADER, "SHAKE_PROBE_STEP_MS")
MILLI = read_define(Path(__file__).resolve().parents[4] / "packages" / "math" / "include" / "math" / "scalar" / "units.h",
                    "MATH_MS_PER_S")
STEP_ENUM = re.search(r"typedef enum\s*\{([^}]+)\}\s*shake_probe_step_t", HEADER.read_text())[1]
STEP_NAMES = tuple(re.findall(r"SHAKE_PROBE_(\w+)", STEP_ENUM))
STEPS = tuple(name for name in STEP_NAMES if name not in ("IDLE", "DONE"))
CANDIDATES = ("size", "smooth", "old")
SHAKING = ("GENTLE", "HARD")
VECTOR = r"(-?\d+),(-?\d+),(-?\d+)"
SAMPLE = re.compile(r"shake (\w+) (\d+) acc " + VECTOR + r" lin " + VECTOR
                    + r" size (\d+) smooth (\d+)(?: jerky \d+)? old (\d+)")
PERCENT = 100
P95 = 95
P99 = 99


def parse(lines):
    samples = []
    for line in lines:
        if "LABDUMP_TRUNCATED" in line:
            raise ValueError("transcript truncated")
        if "round shake" in line:
            samples = []
        match = SAMPLE.fullmatch(line.strip().removeprefix("LABDUMP "))
        if match:
            step = match[1]
            if step.isdigit():
                index = int(step)
                step = STEP_NAMES[index] if 0 <= index < len(STEP_NAMES) else ""
            if step not in STEPS:
                continue
            values = list(map(int, match.groups()[1:]))
            samples.append(dict(step=step, ms=values[0], acc=values[1:4], lin=values[4:7],
                                **dict(zip(CANDIDATES, values[7:]))))
    return samples


def read_capture(path):
    opener = gzip.open if path.suffix == ".gz" else open
    with opener(path, "rt", encoding="utf-8") as stream:
        return parse(stream)


def generate_results(doc, images, captures, check=False):
    changed = False
    for name, path in captures.items():
        samples = read_capture(path)
        stats = summarize(samples)
        for candidate in CANDIDATES:
            suggest(stats, candidate)
        rows = [(step, candidate.upper(), *(f"{value:.1f}" for value in stats[step][candidate].values()))
                for step in STEPS for candidate in CANDIDATES]
        table = markdown(("Step", "Candidate", "Median", "p95", "p99", "Max"), rows)
        body = "<details>\n<summary>Per-step statistics (mg; OLD 0-255)</summary>\n\n" + table + "\n</details>"
        changed |= replace_block(doc, "shake-" + name, body, check=check)
        if not check:
            plot(samples, images / ("shake-" + name + ".png"))
    return changed


def summarize(samples):
    result = {}
    for step in STEPS:
        rows = [sample for sample in samples if sample["step"] == step]
        if not rows:
            continue
        result[step] = {}
        for candidate in CANDIDATES:
            values = [sample[candidate] for sample in rows]
            cuts = (quantiles(values, n=PERCENT, method="inclusive") if len(values) > 1
                    else values * (PERCENT - 1))
            result[step][candidate] = {"median": median(values), "p95": cuts[P95 - 1],
                                       "p99": cuts[P99 - 1], "max": max(values)}
    return result


def suggest(stats, candidate):
    missing = set(STEPS) - stats.keys()
    if missing:
        raise ValueError("missing steps: " + ", ".join(sorted(missing)))
    threshold = max(stats[step][candidate]["p99"] for step in STEPS if step not in SHAKING)
    return (threshold, stats["GENTLE"][candidate]["median"] - threshold,
            stats["HARD"][candidate]["p95"])


def plot(samples, output):
    from docs.plot_style import subplots, save, style_axes
    import matplotlib.pyplot as plt

    figure, axes = subplots(len(CANDIDATES), 1, (12, 9), sharex=True)
    colors = plt.get_cmap("tab10")
    for axis, candidate in zip(axes, CANDIDATES):
        for index, step in enumerate(STEPS):
            rows = [s for s in samples if s["step"] == step]
            axis.plot([s["ms"] / MILLI for s in rows], [s[candidate] for s in rows],
                      color=colors(index), label=step)
        style_axes(axis, candidate.upper())
        axis.set_ylim(bottom=0)
        axis.set_ylabel(candidate.upper() + (" (0-255)" if candidate == "old" else " (mg)"))
        axis.grid(alpha=0.2)
    axes[0].legend(ncol=len(STEPS) // 2, loc="lower center", bbox_to_anchor=(0.5, 1.05))
    axes[-1].set_xlabel("Round time (s)")
    save(figure, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, nargs="?")
    parser.add_argument("--results", action="store_true", help="regenerate retained capture figures and README tables")
    parser.add_argument("--check", action="store_true", help="check generated tables without writing")
    parser.add_argument("--plot", type=Path)
    args = parser.parse_args()
    if args.results:
        app = HEADER.parent
        captures = {name: app / "tools/data" / ("shake-" + name + ".log.gz") for name in ("mixed", "side")}
        return int(generate_results(app / "README.md", app / "images", captures, check=args.check) and args.check)
    if args.log is None:
        parser.error("a capture or --results is required")
    try:
        samples = read_capture(args.log)
        stats = summarize(samples)
        suggestions = {candidate: suggest(stats, candidate) for candidate in CANDIDATES}
    except ValueError as error:
        parser.error(str(error))
    print("step candidate median p95 p99 max (mg; OLD 0-255)")
    for step in STEPS:
        for candidate in CANDIDATES:
            print(step, candidate, *(f"{value:.1f}" for value in stats[step][candidate].values()))
    for candidate, (threshold, margin, full) in suggestions.items():
        print(f"{candidate}: threshold {threshold:.1f}; GENTLE median margin {margin:.1f}; full {full:.1f}")
    if args.plot:
        plot(samples, args.plot)


if __name__ == "__main__":
    raise SystemExit(main())
