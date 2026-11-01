"""Extract seeded voice frame-time comparisons and generate their documentation.

python launcher/tools/audio/sound_gate_tables.py extract COMPARE_DIR
python launcher/tools/audio/sound_gate_tables.py --write
python launcher/tools/audio/sound_gate_tables.py --check
"""
import argparse
from collections import Counter
import json
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
sys.path.insert(0, str(ROOT / "launcher/tools/perf"))
from build.c_header import read_define
from frame_cost import EXTERNAL, REPORT_PREFIX
from perf_compare import observations
from capture import capture_board, require_same_board
from device_report import find_manifest_entry, read_capture_text
from doc_tables import markdown
from generated_blocks import replace_blocks
sys.path.insert(0, str(Path(__file__).resolve().parent))
from audio_doc_blocks import caption, number

DATA = ROOT / "docs/audio/data/sound_gate.json"
DOCUMENT = ROOT / "docs/audio/Mixer-Cost.md"
REGENERATE = "python launcher/tools/audio/sound_gate_tables.py --write"
PERCENT = 100
ROW_FIELDS = ("a", "b", "ratio", "interval", "verdict")
SOUND_MIX_NAME = read_define(ROOT / "launcher/main/sound/sound_engine.h", "SOUND_MIXER_FRAME_COST_NAME")


def capture_date(path):
    for folder in path.resolve().parents:
        index = folder / "index.jsonl"
        if index.is_file():
            entry = find_manifest_entry(path, index)
            if entry and entry.get("started_at"):
                return entry["started_at"].split("T")[0]
            break
    raise ValueError(f"missing capture date in records index: {path}")


def extract(directory, allow_incomplete=False, command=None, label_a=None, label_b=None):
    comparison = json.loads((directory / "comparison.json").read_text(encoding="utf-8"))
    if comparison["incomplete"] and not allow_incomplete:
        raise ValueError("comparison is incomplete; use --allow-incomplete to extract it")
    result = {key: comparison[key] for key in
              ("incomplete", "rng_seed", "threshold", "alpha", "max_seeds")}
    for key, override in (("command", command), ("label_a", label_a), ("label_b", label_b)):
        value = override if override is not None else comparison.get(key)
        if not isinstance(value, str) or not value.strip():
            raise ValueError(f"missing {key}; pass --{key.replace('_', '-')} for an older comparison")
        result[key] = value
    result["rows"] = {name: {key: row[key] for key in ROW_FIELDS}
                      for name, row in sorted(comparison["rows"].items())}
    sides, boards, dates = {}, {}, set()
    for side in ("a", "b"):
        records = [json.loads(path.read_text(encoding="utf-8"))
                   for path in sorted((directory / side).glob("flash_*.json"))]
        if not records:
            raise ValueError(f"missing flash records for side {side}")
        observations(records)
        plans = [item for item in comparison["plan"] if item["side"] == side]
        setups = {tuple(item["console_send"]) for item in plans}
        if len(setups) != 1:
            raise ValueError(f"side {side} must have one consistent console setup")
        if not comparison["incomplete"] and sorted(item["seed"] for item in plans) != sorted(
                record["seed"] for record in records):
            raise ValueError(f"side {side} flash seeds do not match the plan")
        weighted, worst, blocks, suites = 0, 0, 0, set()
        seen = set()
        for record in records:
            boards[f"{side}/{record['seed']}"] = record["board"]
            for suite, entry in record["suites"].items():
                for run in entry["runs"]:
                    for raw in run.get("captures", [run["capture"]]):
                        path = Path(raw)
                        if not path.is_absolute():
                            path = directory / path
                        path = path.resolve()
                        if path in seen:
                            continue
                        seen.add(path)
                        boards[str(path)] = capture_board(path)
                        dates.add(capture_date(path))
                        for line in read_capture_text(path).splitlines():
                            if REPORT_PREFIX not in line:
                                continue
                            for name, average, peak, count in EXTERNAL.findall(line):
                                if name != SOUND_MIX_NAME:
                                    continue
                                count = int(count)
                                weighted += float(average) * count
                                worst = max(worst, float(peak))
                                blocks += count
                                suites.add(suite)
        sides[side] = dict(seeds=[record["seed"] for record in records],
                           build_ids=[record["build_id"] for record in records],
                           console_send=list(next(iter(setups))),
                           **{SOUND_MIX_NAME: dict(average_ms=weighted / blocks if blocks else None,
                                          worst_ms=worst if blocks else None,
                                          blocks=blocks, suites=sorted(suites))})
    first_plans = [next(item for item in comparison["plan"] if item["side"] == side) for side in ("a", "b")]
    if first_plans[0]["suites"] != first_plans[1]["suites"]:
        raise ValueError("sides must start with the same suite selection")
    result.update(board=require_same_board(boards), capture_date=sorted(dates), sides=sides)
    return result


def delta(ratio):
    return (ratio - 1) * PERCENT


def render(path):
    data = json.loads(path.read_text(encoding="utf-8"))
    prefix = caption(path, REGENERATE)
    header = (f"Seed cap {data['max_seeds']} per side; threshold \u00b1{data['threshold']:g}%; "
              f"alpha {data['alpha']:g}.\n\n")
    if data["incomplete"]:
        header += "**Incomplete comparison.**\n\n"
    rows = list(data["rows"].values())
    verdicts = sorted({row["verdict"] for row in rows})
    counts = Counter(row["verdict"] for row in rows)
    worst = max((row for row in rows if row["ratio"] is not None),
                key=lambda row: row["ratio"], default=None)
    interval = worst["interval"] if worst else None
    summary = [(sum(row["a"] is not None and row["b"] is not None for row in rows),
                *(counts[verdict] for verdict in verdicts),
                number(delta(worst["ratio"]) if worst else None),
                ", ".join(number(delta(bound)) for bound in interval) if interval else "n/a",
                sum(row["interval"] is not None and row["interval"][0] > 1 for row in rows))]
    tables = {"sound-gate": prefix + header + markdown(
        ["Rows measured", *verdicts, "Worst Delta time %",
         "Worst row interval (Delta %)", "Intervals wholly above zero"], summary)}
    mix_rows = []
    for side in ("a", "b"):
        mix = data["sides"][side][SOUND_MIX_NAME]
        mix_rows.append((data["label_" + side], number(mix["average_ms"]), number(mix["worst_ms"]), mix["blocks"]))
    tables["sound-gate-mix"] = prefix + markdown(
        ["Arm", "Average ms/block", "Worst ms/block", "Blocks"], mix_rows)
    return tables


def update(data, document, check=False):
    return replace_blocks(document, render(data), check=check)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", choices=["extract"])
    parser.add_argument("directory", nargs="?", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--write", action="store_true")
    mode.add_argument("--check", action="store_true")
    parser.add_argument("--allow-incomplete", action="store_true")
    parser.add_argument("--command")
    parser.add_argument("--label-a")
    parser.add_argument("--label-b")
    parser.add_argument("--data", type=Path, default=DATA)
    parser.add_argument("--document", type=Path, default=DOCUMENT)
    args = parser.parse_args()
    if bool(args.action) == bool(args.write or args.check) or (args.action and not args.directory):
        parser.error("choose extract COMPARE_DIR, --write or --check")
    try:
        if args.action:
            data = extract(args.directory, args.allow_incomplete, args.command, args.label_a, args.label_b)
            args.data.parent.mkdir(parents=True, exist_ok=True)
            args.data.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
            print(f"sound gate: extracted {len(data['rows'])} rows to {args.data}")
            return 0
        changed = update(args.data, args.document, args.check)
    except (ValueError, KeyError, OSError) as error:
        parser.exit(2, f"sound gate: {error}\n")
    print(f"sound gate: {'stale' if changed and args.check else 'updated' if changed else 'current'}")
    return int(args.check and changed)


if __name__ == "__main__":
    raise SystemExit(main())
