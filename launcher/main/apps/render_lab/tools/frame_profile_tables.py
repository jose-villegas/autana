"""Generate frame timing tables and a pose chart from committed board captures."""
import argparse
import json
import pathlib
import shlex
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
TOOLS = ROOT / "launcher/tools"
sys.path[:0] = [str(TOOLS), str(TOOLS / "perf"), str(TOOLS / "render")]
from frame_profile import profiles, poses
from frame_cost import US_PER_MS, MS_PER_SECOND
from generated_blocks import replace_block
from capture import QUARTERS, build_id
from doc_stages import capture_pose_every_ms

SCRIPT = pathlib.Path(__file__).resolve().relative_to(ROOT).as_posix()
DOCUMENT = "launcher/main/apps/render_lab/tools/README.md"
DATA = "docs/render/data"
BLOCK = "frame-timing-profiles"
PALETTE = (("Blue", "#2563eb"), ("Orange", "#ea580c"))
PROFILE_PREFIX = "frame-profile-"
BOARD_SUFFIX = "-board"
PROFILE_GLOB = f"{PROFILE_PREFIX}*{BOARD_SUFFIX}.log"


def capture_text(path):
    text = path.read_text(encoding="utf-8")
    try:
        build = build_id(text)
    except ValueError as error:
        raise ValueError(f"{path}: {error}") from error
    return text, build


def timing_tables(data_dir, scenes, pose_row, boards, pose_capture, pose_board=None):
    captures = ({board: data_dir / f"{PROFILE_PREFIX}{board}{BOARD_SUFFIX}.log" for board in boards}
                if boards else {path.stem.removeprefix(PROFILE_PREFIX).removesuffix(BOARD_SUFFIX): path
                                for path in sorted(data_dir.glob(PROFILE_GLOB))})
    if not captures:
        raise ValueError(f"{data_dir}: no {PROFILE_GLOB} captures")
    builds = {}
    readings = {}
    for board, path in captures.items():
        text, builds[path] = capture_text(path)
        entries = profiles(text)
        if not entries:
            raise ValueError(f"{path}: no playback profiles")
        indexed = {(row["row"], row["quarter"]): row for row in entries}
        if len(indexed) != len(entries):
            raise ValueError(f"{path}: duplicate profile")
        readings[board] = indexed
    if scenes is None:
        scenes = list(dict.fromkeys(row for row, quarter in next(iter(readings.values()))))
    lines = ["### Measured frame rate", "",
             f"Cells list {' / '.join(board.upper() for board in captures)}.",
             "Playback includes update, render, compose and present.", "",
             "| Scene | Quarter | Mean fps | p50 ms | p95 ms | Worst ms |",
             "|---|---|---:|---:|---:|---:|"]
    for scene in scenes:
        for quarter in QUARTERS:
            cells = [scene, quarter]
            for field in ("fps_mean", "med", "p95", "worst"):
                scale = 1 if field == "fps_mean" else US_PER_MS
                try:
                    cells.append(" / ".join(f"{readings[board][scene, quarter][field] / scale:.1f}" for board in captures))
                except KeyError as error:
                    raise ValueError(f"missing profile: {scene} {quarter}") from error
            lines.append("| " + " | ".join(cells) + " |")
    text, builds[pose_capture] = capture_text(pose_capture)
    samples = [row for row in poses(text) if row["row"] == pose_row]
    every_ms = capture_pose_every_ms([pose_capture])
    board = pose_board or pose_capture.stem.removesuffix(BOARD_SUFFIX).rsplit("-", 1)[-1]
    quarters = {quarter: [row for row in samples if row["quarter"] == quarter] for quarter in QUARTERS}
    times = [row["time_ms"] for row in quarters[QUARTERS[0]]]
    if len(times) < 2 or times != sorted(set(times)) or any(
            [row["time_ms"] for row in rows] != times for rows in quarters.values()):
        raise ValueError("pose quarters need matching, increasing sample times")
    lines.extend(("", "### Frame cost along the flythrough", "",
                  f"`{pose_row}` on {board.upper()}, before presentation, sampled every "
                  f"{every_ms / MS_PER_SECOND:g} s.",
                  "; ".join(f"{colour} is {quarter}" for quarter, (colour, _) in zip(QUARTERS, PALETTE)) + ".",
                  "", "```mermaid",
                  "%%{init: " + json.dumps({"themeVariables": {"xyChart": {
                      "plotColorPalette": ", ".join(colour for _, colour in PALETTE)}}}) + "}%%",
                  "xychart-beta", f'    x-axis "Clip time (s)" {times[0] / MS_PER_SECOND:g} --> {times[-1] / MS_PER_SECOND:g}',
                  '    y-axis "Frame ms"'))
    for rows in quarters.values():
        lines.append("    line [" + ", ".join(f"{row['frame_us'] / US_PER_MS:.1f}" for row in rows) + "]")
    lines.extend(("```", "", "Sources:", ""))
    lines.extend(f"- `{path.name}`: build `{builds[path]}`." for path in (*captures.values(), pose_capture))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=pathlib.Path, default=ROOT / DATA)
    parser.add_argument("--document", type=pathlib.Path, default=ROOT / DOCUMENT)
    parser.add_argument("--scenes", help="comma-separated row order; defaults to capture order")
    parser.add_argument("--board", action="append", help="board id; defaults to all playback captures")
    parser.add_argument("--pose-capture", type=pathlib.Path, required=True)
    parser.add_argument("--pose-board", help="board label; defaults to the pose capture filename")
    parser.add_argument("--pose-row", required=True)
    parser.add_argument("--check", action="store_true")
    argv = sys.argv[1:]
    args = parser.parse_args(argv)
    command = shlex.join(["python", SCRIPT, *(arg for arg in argv if arg != "--check"), "--check"])
    try:
        body = timing_tables(args.data_dir, args.scenes.split(",") if args.scenes else None,
                             args.pose_row, args.board, args.pose_capture, args.pose_board)
        changed = replace_block(args.document, BLOCK, body, check=args.check, command=command)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.check and changed:
        print(f"{args.document}: {BLOCK} differs; run the generator")
    return int(args.check and changed)


if __name__ == "__main__":
    sys.exit(main())
