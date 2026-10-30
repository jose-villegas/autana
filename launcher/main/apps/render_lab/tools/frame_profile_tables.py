"""Generate frame timing tables, and each scene's frames along its path beside
what the cull counted, from committed board captures."""
import argparse
import pathlib
import shlex
import sys

ROOT = pathlib.Path(__file__).resolve().parents[5]
TOOLS = ROOT / "launcher/tools"
sys.path[:0] = [str(TOOLS), str(TOOLS / "perf"), str(TOOLS / "render")]
from frame_profile import judged, profiles, trace
from frame_cost import US_PER_MS, MS_PER_SECOND
from generated_blocks import replace_block
from capture import QUARTERS, build_id
from cull_counts import plot_counts
from docs.plot_style import INK, LABEL_SIZE, MUTED, SLOTS, save, style_axes, subplots
from report_performance import frame_peaks, path_poses

SCRIPT = pathlib.Path(__file__).resolve().relative_to(ROOT).as_posix()
DOCUMENT = "launcher/main/apps/render_lab/tools/README.md"
DATA = "docs/render/data"
IMAGES = "launcher/main/apps/render_lab/tools/images/frame-peaks"
BLOCK = "frame-timing-profiles"
FIGURE_SIZE = (11, 8)
FRAME_DOT = 4  # marker area of each played frame
PEAK_DOT = 4 * FRAME_DOT
RANK_OFFSET = (3, 3)  # points from a frame peak to its rank's label
POINTS_PER_INCH = 72
PROFILE_PREFIX = "frame-profile-"
BOARD_SUFFIX = "-board"
PROFILE_GLOB = f"{PROFILE_PREFIX}*{BOARD_SUFFIX}.log"


def capture_text(path):
    text = path.read_text(encoding="utf-8")
    try:
        build = build_id(text)
    except ValueError as error:
        raise ValueError(f"{path}: {error}") from error
    return judged(text), build


def path_chart(row, frames, poses, profile, out):
    """One column per quarter: the played frames' ms over path time, the frame peaks ranked and p95 drawn,
    above the cull's cluster and triangle counts at the evenly replayed poses."""
    fig, axes = subplots(3, len(QUARTERS), FIGURE_SIZE, sharex="col")
    for column, quarter in enumerate(QUARTERS):
        played = [frame for frame in frames if frame["quarter"] == quarter]
        path, ranked = poses[row, quarter]
        timing = axes[0][column]
        timing.scatter([f["t_ms"] / MS_PER_SECOND for f in played], [f["frame_us"] / US_PER_MS for f in played],
                       s=FRAME_DOT, color=SLOTS[0], label="played frame")
        p95 = profile[row, quarter]["p95"] / US_PER_MS
        timing.axhline(p95, color=MUTED, linestyle="--", linewidth=1, label=f"p95 {p95:.1f} ms")
        for index, (frame, _) in enumerate(ranked):
            point = (frame["t_ms"] / MS_PER_SECOND, frame["frame_us"] / US_PER_MS)
            timing.scatter(*point, s=PEAK_DOT, color=SLOTS[1], label="frame peak" if index == 0 else None)
            timing.annotate(str(frame["rank"]), point, xytext=RANK_OFFSET, textcoords="offset points",
                            fontsize=LABEL_SIZE, color=INK)
        style_axes(timing, f"{row}, {quarter}: frame ms, peaks")
        timing.legend(frameon=False, fontsize="small", loc="lower right")
        plot_counts(axes[1][column], axes[2][column], [pose["t_ms"] / MS_PER_SECOND for pose in path],
                    [pose["counts"] for pose in path], f"{row}, {quarter}", legend=column == 0)
        axes[2][column].set_xlabel("seconds along the path")
    fig.canvas.draw()  # lays the panels out, so each knows its height
    for column, quarter in enumerate(QUARTERS):
        timing = axes[0][column]
        # Room above the slowest frame for its rank's label.
        stack = RANK_OFFSET[1] + LABEL_SIZE
        height = timing.get_window_extent().height / fig.dpi * POINTS_PER_INCH
        slowest = max(f["frame_us"] for f in frames if f["quarter"] == quarter) / US_PER_MS
        timing.set_ylim(0, slowest / (1 - stack / height))
    save(fig, out)


def timing_tables(data_dir, scenes, boards, peak_board, images, write_images):
    captures = ({board: data_dir / f"{PROFILE_PREFIX}{board}{BOARD_SUFFIX}.log" for board in boards}
                if boards else {path.stem.removeprefix(PROFILE_PREFIX).removesuffix(BOARD_SUFFIX): path
                                for path in sorted(data_dir.glob(PROFILE_GLOB))})
    if not captures:
        raise ValueError(f"{data_dir}: no {PROFILE_GLOB} captures")
    if peak_board not in captures:
        raise ValueError(f"no {PROFILE_PREFIX}{peak_board}{BOARD_SUFFIX}.log among the captures")
    readings, texts = {}, {}
    for board, path in captures.items():
        text, _ = capture_text(path)
        texts[board] = text
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
             f"Cells list {' / '.join(f'board {number}' for number in range(1, len(captures) + 1))}.",
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
    text = texts[peak_board]
    poses = path_poses(text)
    frames = trace(text)
    lines.extend(("", "### Frame peaks along the path", "",
                  f"Board {list(captures).index(peak_board) + 1}. Each scene's played frames over path time, "
                  "its frame peaks ranked,",
                  "above what the cull counted at poses replayed paused along the path.", ""))
    folder = images.relative_to(ROOT).relative_to(pathlib.PurePath(DOCUMENT).parent).as_posix()
    for scene in scenes:
        if not all((scene, quarter) in poses for quarter in QUARTERS):
            raise ValueError(f"{captures[peak_board].name}: no frame peaks for {scene}")
        if write_images:
            path_chart(scene, [f for f in frames if f["row"] == scene], poses, readings[peak_board],
                       images / f"{scene}.png")
        lines.append(f"![{scene}: frames along the path and the cull's counts]({folder}/{scene}.png)")
    lines.extend(("", frame_peaks(text).replace("## Frame peaks", "#### Frame peaks, ranked", 1)))
    lines.extend(("Sources:", ""))
    lines.extend(f"- `{path.name}`" for path in captures.values())
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=pathlib.Path, default=ROOT / DATA)
    parser.add_argument("--document", type=pathlib.Path, default=ROOT / DOCUMENT)
    parser.add_argument("--scenes", help="comma-separated row order; defaults to capture order")
    parser.add_argument("--board", action="append", help="capture label in frame-profile-<label>-board.log; "
                        "defaults to every playback capture")
    parser.add_argument("--peak-board", required=True,
                        help="the capture label whose traced frames and replays draw the frame peaks")
    parser.add_argument("--images", type=pathlib.Path, default=ROOT / IMAGES)
    parser.add_argument("--check", action="store_true")
    argv = sys.argv[1:]
    args = parser.parse_args(argv)
    command = shlex.join(["python", SCRIPT, *(arg for arg in argv if arg != "--check"), "--check"])
    try:
        body = timing_tables(args.data_dir, args.scenes.split(",") if args.scenes else None,
                             args.board, args.peak_board, args.images.resolve(), not args.check)
        changed = replace_block(args.document, BLOCK, body, check=args.check, command=command)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if args.check and changed:
        print(f"{args.document}: {BLOCK} differs; run the generator")
    return int(args.check and changed)


if __name__ == "__main__":
    sys.exit(main())
