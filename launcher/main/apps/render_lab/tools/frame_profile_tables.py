"""Generate frame timing tables and a pose chart from committed board captures."""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools/perf"))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[4] / "tools/render"))
from frame_profile import profiles, poses
from frame_cost import US_PER_MS, MS_PER_SECOND
from generated_blocks import replace_block
from capture import QUARTERS

ROOT = pathlib.Path(__file__).resolve().parents[5]
DOCUMENT = "launcher/main/apps/render_lab/tools/README.md"
DATA = "docs/render/data"
BOARDS = ("a308", "20e8", "2124")
BLOCK = "frame-timing-profiles"
CHECK_COMMAND = ("python launcher/main/apps/render_lab/tools/frame_profile_tables.py "
                 "--pose-row atrium_fitted_full "
                 "--scenes sponza-fitted-full,sponza-fitted,sponza,sponza-lite,sponza-flat,sponza-flat-fitted,capybara --check")


def timing_tables(data_dir, scenes, pose_row):
    captures = {board: data_dir / f"frame-profile-{board}-board.log" for board in BOARDS}
    pose_capture = data_dir / f"sponza-perf-{BOARDS[0]}-board.log"
    readings = {}
    for board, path in captures.items():
        entries = profiles(path.read_text(encoding="utf-8"))
        indexed = {(row["row"], row["quarter"]): row for row in entries}
        if len(indexed) != len(entries):
            raise ValueError(f"{path}: duplicate profile")
        readings[board] = indexed
    lines = ["### Measured frame rate", "",
             "Cells list A308 / 20E8 / 2124. Playback includes update, render, compose and present.", "",
             "| Scene | Quarter | Mean fps | p50 ms | p95 ms | Worst ms |",
             "|---|---|---:|---:|---:|---:|"]
    for scene in scenes:
        for quarter in QUARTERS:
            cells = [scene, quarter]
            for field in ("fps_mean", "med", "p95", "worst"):
                scale = 1 if field == "fps_mean" else US_PER_MS
                try:
                    cells.append(" / ".join(f"{readings[board][scene, quarter][field] / scale:.1f}" for board in BOARDS))
                except KeyError as error:
                    raise ValueError(f"missing profile: {scene} {quarter}") from error
            lines.append("| " + " | ".join(cells) + " |")
    samples = [row for row in poses(pose_capture.read_text(encoding="utf-8")) if row["row"] == pose_row]
    quarters = {quarter: [row for row in samples if row["quarter"] == quarter] for quarter in QUARTERS}
    times = [row["time_ms"] for row in quarters[QUARTERS[0]]]
    if len(times) < 2 or times != sorted(set(times)) or any(
            [row["time_ms"] for row in rows] != times for rows in quarters.values()):
        raise ValueError("pose quarters need matching, increasing sample times")
    lines.extend(("", "### Frame cost along the flythrough", "",
                  f"`{pose_row}` on A308, before presentation, sampled every "
                  f"{(times[1] - times[0]) / MS_PER_SECOND:g} s.",
                  "Blue is portrait; orange is landscape.", "", "```mermaid",
                  '%%{init: {"themeVariables": {"xyChart": {"plotColorPalette": "#2563eb, #ea580c"}}}}%%',
                  "xychart-beta", f'    x-axis "Clip time (s)" {times[0] / MS_PER_SECOND:g} --> {times[-1] / MS_PER_SECOND:g}',
                  '    y-axis "Frame ms"'))
    for rows in quarters.values():
        lines.append("    line [" + ", ".join(f"{row['frame_us'] / US_PER_MS:.1f}" for row in rows) + "]")
    lines.extend(("```", "", "Sources in `docs/render/data/`:", ""))
    lines.extend(f"- `{path.name}`" for path in (*captures.values(), pose_capture))
    lines.extend(("", "Regenerate with:", "", "```sh", CHECK_COMMAND.removesuffix(" --check"), "```"))
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=pathlib.Path, default=ROOT / DATA)
    parser.add_argument("--document", type=pathlib.Path, default=ROOT / DOCUMENT)
    parser.add_argument("--scenes", required=True)
    parser.add_argument("--pose-row", required=True)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    try:
        body = timing_tables(args.data_dir, args.scenes.split(","), args.pose_row)
        changed = replace_block(args.document, BLOCK, body, check=args.check, command=CHECK_COMMAND)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.check and changed:
        print(f"{args.document}: {BLOCK} differs; run the generator")
    return int(args.check and changed)


if __name__ == "__main__":
    sys.exit(main())
