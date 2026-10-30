"""LNT v2 timing reports and board-local night manifests."""
from dataclasses import asdict
import json
from pathlib import Path

MICROSECONDS_PER_SECOND = 1_000_000
# Build variant and options every nightly capture uses.
VARIANT = "diag"
LAYOUT_SEED = 0
BUILD_FLAGS = ("--perf-scope", "--layout-seed", str(LAYOUT_SEED))
IMAGE = " ".join((VARIANT, *BUILD_FLAGS))


def lnt_report(machine, board, commit, pr, role, night, build_id, image_hash, start, end, rows):
    return {
        "format_version": "2",
        "machine": {"name": machine, "board": board, "image": IMAGE},
        "run": {"start_time": start, "end_time": end, "llvm_project_revision": commit,
                "pr": pr, "build_id": build_id, "image_hash": image_hash, "night": night, "role": role},
        "tests": [{"name": name, "execution_time": [value / MICROSECONDS_PER_SECOND for value in values]}
                  for name, values in rows.items()],
    }


def night_manifest(board, machine, night, lane, results, stop_reason, last_measured, selftest, started, ended):
    steps = []
    for result in results:
        entry = asdict(result)
        entry.update(entry.pop("step"))
        steps.append(entry)
    return dict(board=board, machine=machine, night=night, lane=lane, steps=steps,
                stop_reason=stop_reason, last_measured=last_measured, selftest=selftest, started=started, ended=ended)


def night_ran(manifest):
    """Whether a lane ran its self-test and chain to the end, whatever its tests reported: its board
    was up. A missing or unfinished manifest, a self-test that never completed or a stop other than
    another job taking the board means the board could not run."""
    return (bool(manifest and manifest.get("ended")) and manifest.get("stop_reason") in (None, "busy")
            and manifest.get("selftest", {}).get("outcome") != "incomplete")


def read_night(data_dir, board, key):
    """A lane's manifest for the night keyed `key`, or None."""
    path = Path(data_dir) / "scoped" / board / key / "night.json"
    return json.loads(path.read_text(encoding="utf-8")) if path.is_file() else None


def read_last_night(data_dir, board):
    paths = sorted((Path(data_dir) / "scoped" / board).glob("????-??-??-????/night.json"))
    return json.loads(paths[-1].read_text(encoding="utf-8")) if paths else None
