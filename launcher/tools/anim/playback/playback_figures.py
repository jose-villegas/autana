"""The scene playback figures of docs/Animation-Tracks.md: clip time against app time as wrap, speed and pause
change it; a transition blend and an interrupted one, as weight and as one joint's turn; nlerp beside slerp;
and the skeleton itself through those blends, animated. The numbers come from playback_figures_probe.c, which
plays a skinned glTF's clips, baked by the pack's own writers, through anim_layer_t.

    python launcher/tools/anim/playback/playback_figures.py MODEL.glb FIRST SECOND THIRD --cc CC --out DIR

FIRST, SECOND and THIRD name three of the model's clips: the blend runs FIRST, SECOND, THIRD, then FIRST again,
and the playhead panels play SECOND. DIR receives anim-playhead.png, anim-blend.png, anim-nlerp.png and anim-blend.gif.
launcher/tools/render/render_doc_images.sh runs it.
"""

import argparse
import io
import math
import pathlib
import sys
import tempfile

from PIL import Image

TOOLS = pathlib.Path(__file__).resolve().parents[2]
MAIN = TOOLS.parent / "main"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "math"))
from math_figures import run_probe  # noqa: E402
from anim import skeleton_asset, tracks_asset  # noqa: E402
from asset.asset_pack import build_pack  # noqa: E402
from docs.plot_style import FAINT, INK, LABEL_SIZE, MUTED, SLOTS, TITLE_SIZE, figure as new_figure, save, subplots  # noqa: E402
from gltf import gltf_read  # noqa: E402
import matplotlib.pyplot as plt  # noqa: E402  (after plot_style, which picks the headless backend)
from matplotlib.lines import Line2D  # noqa: E402

PROBE = pathlib.Path(__file__).with_name("playback_figures_probe.c")
PROBE_SOURCES = (PROBE, *[MAIN / name for name in (
    "anim/anim_track.c", "anim/anim_tracks.c", "anim/anim_binding.c", "anim/anim_skeleton.c",
    "anim/pose/anim_pose.c", "anim/playback/anim_playhead.c", "anim/playback/anim_layer.c",
    "asset/asset_pack.c", "asset/asset_file.c")])

# The scripts' clip indices, into the three clips named on the command line.
FIRST, SECOND, THIRD = range(3)
FRAME_MS = 20
BLEND_MS = 400
# Wrap, speed and pause, on the second clip: (time ms, event) lines of the probe's script.
PLAYHEAD_END_MS = 3000
PLAYHEAD_RUNS = {
    "loop": [(0, f"play {SECOND} 0")],
    "clamp": [(0, "wrap 1"), (0, f"play {SECOND} 0")],
    "speed": [(0, f"play {SECOND} 0"), (1000, "speed 0 0.5"), (1800, "pause 0 1"), (2200, "pause 0 0"),
              (2200, "speed 0 2")],
}
# A blend from the first clip to the second, then to the third, interrupted half way by one back to the first.
INTERRUPT_AT = 2000
BLEND_END_MS = 3200
BLEND_SCRIPT = [(0, f"play {FIRST} 0"), (800, f"play {SECOND} {BLEND_MS}"), (1800, f"play {THIRD} {BLEND_MS}"),
                (INTERRUPT_AT, f"play {FIRST} {BLEND_MS}")]
# Room above weight 1 for the play labels.
WEIGHT_TOP = 1.45
NLERP_DEGREES = 150
NLERP_STEPS = 60
PANEL = (4.2, 2.6)  # inches per panel
SHOWN, CURRENT, PREVIOUS = SLOTS[0], SLOTS[1], SLOTS[2]
# The GIF: one image every GIF_STEP frames, played at the clip's own speed, turning ORBIT_DEGREES over the run.
GIF_STEP = 2
GIF_SIZE = (4.4, 3.6)
GIF_DPI = 80
ORBIT_DEGREES = 40
GIF_ZOOM = 1.5  # the 3D box fills more of the frame with its axes hidden
VIEW_ELEVATION, VIEW_AZIMUTH = 15, -70


def model_pack(model, clips, directory):
    """The model's skeleton and clips as SKEL and TRCK entries, written by the pack's own writers."""
    document, binary = gltf_read.load_glb(model)
    mesh_node = next(i for i, n in enumerate(document["nodes"]) if "skin" in n and "mesh" in n)
    _, rows, skeleton = skeleton_asset.rig(document, mesh_node)
    entries = [(skeleton, b"SKEL", skeleton_asset.encode(rows))]
    for name in clips:
        tracks, duration, root = tracks_asset.clip_tracks(document, binary,
                                                           tracks_asset.find_animation(document, name, model))
        entries.append((name, b"TRCK", tracks_asset.encode(tracks, duration, root)))
    pack = directory / "clips.pack"
    pack.write_bytes(build_pack(entries))
    return pack, skeleton


def play(cc, pack, skeleton, clips, events, end_ms, directory):
    script = directory / "script.txt"
    lines = [f"clips {len(clips)} " + " ".join(clips), f"frame {FRAME_MS} {end_ms}"]
    lines += [f"{at} {event}" for at, event in events]
    script.write_text("\n".join(lines) + "\n", encoding="utf-8")
    return run_probe(cc, PROBE_SOURCES, pack, skeleton, script)


def turn_degrees(q, rest):
    """How far rotation q is from rest, in degrees, the short way."""
    dot = abs(sum(a * b for a, b in zip(q, rest)))
    return math.degrees(2 * math.acos(min(dot, 1.0)))


def plain_axes(axis, title):
    axis.set_title(title, fontsize=TITLE_SIZE, color=INK)
    axis.tick_params(labelsize=LABEL_SIZE, colors=MUTED)
    for spine in axis.spines.values():
        spine.set_color(FAINT)


def clip_name(run, index):
    return run["clips"][index]["name"]


def mark_events(axis, run, events, top):
    """A dashed line and a label per play call, staggered so neighbours do not overlap."""
    for index, (at, event) in enumerate(events):
        words = event.split()
        if words[0] != "play":
            continue
        axis.axvline(at, color=FAINT, linestyle="--", linewidth=1)
        blend = f", {words[2]} ms" if words[2] != "0" else ""
        axis.text(at + FRAME_MS, top * (0.93 - 0.08 * (index % 2)), f"play {clip_name(run, int(words[1]))}{blend}",
                  fontsize=LABEL_SIZE, color=MUTED)


def playhead_figure(runs, output):
    fig, (wrap_axis, speed_axis) = subplots(1, 2, (2 * PANEL[0], PANEL[1]))
    duration = runs["loop"]["clips"][SECOND]["duration_ms"]
    plain_axes(wrap_axis, f"{clip_name(runs['loop'], SECOND)}, {duration} ms long: loop and clamp")
    for name, colour in (("loop", SHOWN), ("clamp", CURRENT)):
        frames = runs[name]["frames"]
        wrap_axis.plot([f["ms"] for f in frames], [f["current"][1] for f in frames], color=colour, label=name)
    plain_axes(speed_axis, "speed 1, then 0.5, paused, then 2")
    frames = runs["speed"]["frames"]
    speed_axis.plot([f["ms"] for f in frames], [f["current"][1] for f in frames], color=SHOWN)
    for at, event in PLAYHEAD_RUNS["speed"][1:]:
        speed_axis.axvline(at, color=FAINT, linestyle="--", linewidth=1)
    for axis in (wrap_axis, speed_axis):
        axis.axhline(duration, color=FAINT, linewidth=1)
        axis.set_ylim(0, duration * 1.1)
        axis.set_xlim(0, PLAYHEAD_END_MS)
        axis.set_xlabel("app time, ms", fontsize=LABEL_SIZE)
        axis.set_ylabel("clip time, ms", fontsize=LABEL_SIZE)
    wrap_axis.legend(fontsize=LABEL_SIZE, loc="lower right")
    save(fig, output)


def busiest_joint(run):
    """The joint whose shown rotation moves most across the run: the one the blend panel traces."""
    rest = run["frames"][0]["shown"]
    spans = []
    for j in range(len(run["parents"])):
        angles = [turn_degrees(f["shown"][j], rest[j]) for f in run["frames"]]
        spans.append(max(angles) - min(angles))
    return max(range(len(spans)), key=spans.__getitem__)


def blend_figure(run, output):
    frames = run["frames"]
    joint = busiest_joint(run)
    reference = frames[0]["shown"][joint]
    ms = [f["ms"] for f in frames]
    fig, (weight_axis, turn_axis) = subplots(2, 1, (2 * PANEL[0], 2 * PANEL[1]), sharex=True)
    plain_axes(weight_axis, "blend weight toward the current clip")
    weight_axis.plot(ms, [f["weight"] for f in frames], color=SHOWN)
    frozen = [f["ms"] for f in frames if f["frozen"]]
    if frozen:
        weight_axis.axvspan(frozen[0], frozen[-1], color=FAINT, alpha=0.4, label="previous pose frozen")
        weight_axis.legend(fontsize=LABEL_SIZE, loc="lower left")
    weight_axis.set_ylim(0, WEIGHT_TOP)
    mark_events(weight_axis, run, BLEND_SCRIPT, WEIGHT_TOP)

    name = run["paths"][joint].rsplit("/", 1)[-1]
    plain_axes(turn_axis, f"joint {name}: turn from its first pose")
    turn_axis.plot(ms, [turn_degrees(f["current_alone"][joint], reference) for f in frames], color=CURRENT,
                   linestyle="--", label="current clip alone")
    previous = [(f["ms"], turn_degrees(f["previous_pose"][joint], reference)) for f in frames if f["previous_pose"]]
    turn_axis.plot([p[0] for p in previous], [p[1] for p in previous], color=PREVIOUS, linestyle="",
                   marker=".", markersize=3, label="previous slot")
    turn_axis.plot(ms, [turn_degrees(f["shown"][joint], reference) for f in frames], color=SHOWN, label="shown")
    turn_axis.set_ylim(0, None)
    turn_axis.set_xlim(0, BLEND_END_MS)
    turn_axis.set_xlabel("app time, ms", fontsize=LABEL_SIZE)
    turn_axis.set_ylabel("degrees", fontsize=LABEL_SIZE)
    turn_axis.legend(fontsize=LABEL_SIZE, loc="upper left", ncols=3)
    save(fig, output)


def nlerp_figure(data, output):
    fig, (angle_axis, rate_axis) = subplots(1, 2, (2 * PANEL[0], PANEL[1]))
    steps = data["steps"]
    t = [s["t"] for s in steps]
    for key, colour in (("slerp", CURRENT), ("nlerp", SHOWN)):
        angles = [math.degrees(2 * math.atan2(s[key][0], s[key][1])) for s in steps]
        angle_axis.plot(t, angles, color=colour, label=key)
        rates = [(b - a) / (t1 - t0) for a, b, t0, t1 in zip(angles, angles[1:], t, t[1:])]
        rate_axis.plot([(a + b) / 2 for a, b in zip(t, t[1:])], rates, color=colour, label=key)
    plain_axes(angle_axis, f"a {data['degrees']:g} degree turn: angle reached")
    angle_axis.set_ylim(0, data["degrees"] * 1.05)
    angle_axis.set_ylabel("degrees", fontsize=LABEL_SIZE)
    plain_axes(rate_axis, "angular speed")
    rate_axis.set_ylim(0, None)
    rate_axis.set_ylabel("degrees per unit weight", fontsize=LABEL_SIZE)
    for axis in (angle_axis, rate_axis):
        axis.set_xlim(0, 1)
        axis.set_xlabel("blend weight", fontsize=LABEL_SIZE)
        axis.legend(fontsize=LABEL_SIZE, loc="lower right")
    save(fig, output)


def up(point):
    """glTF is +Y up; the plot is +Z up."""
    return point[0], -point[2], point[1]


def describe(run, frame):
    current = clip_name(run, frame["current"][0])
    if frame["frozen"]:
        return f"frozen pose to {current}, weight {frame['weight']:.2f}"
    if frame["previous"] is not None:
        return f"{clip_name(run, frame['previous'][0])} to {current}, weight {frame['weight']:.2f}"
    return current


KEY = [Line2D([], [], color=INK, linewidth=2, label="shown"), Line2D([], [], color=CURRENT, label="current clip alone"),
       Line2D([], [], color=PREVIOUS, label="previous slot")]


def blend_gif(run, output):
    frames = run["frames"][::GIF_STEP]
    points = [up(p) for f in frames for key in ("joints", "current_joints") for p in f[key]]
    low = [min(p[a] for p in points) for a in range(3)]
    high = [max(p[a] for p in points) for a in range(3)]
    half = max(h - l for l, h in zip(low, high)) / 2
    centre = [(l + h) / 2 for l, h in zip(low, high)]
    images = []
    for index, frame in enumerate(frames):
        fig = new_figure(GIF_SIZE)
        axis = fig.add_subplot(projection="3d")
        axis.set_title(f"{frame['ms']} ms: {describe(run, frame)}", fontsize=TITLE_SIZE, color=INK)
        fig.legend(handles=KEY, fontsize=LABEL_SIZE, loc="lower center", ncols=len(KEY), frameon=False)
        ghosts = [("current_joints", CURRENT, 1.0)]
        if frame["previous_joints"] is not None:
            ghosts.append(("previous_joints", PREVIOUS, 1.0))
        for key, colour, width in ghosts + [("joints", INK, 2.0)]:
            joints = [up(p) for p in frame[key]]
            for j, parent in enumerate(run["parents"]):
                if parent >= 0:
                    axis.plot(*zip(joints[parent], joints[j]), color=colour, linewidth=width)
        for a, setter in enumerate((axis.set_xlim, axis.set_ylim, axis.set_zlim)):
            setter(centre[a] - half, centre[a] + half)
        axis.set_axis_off()
        axis.set_box_aspect((1, 1, 1), zoom=GIF_ZOOM)
        axis.view_init(elev=VIEW_ELEVATION, azim=VIEW_AZIMUTH + ORBIT_DEGREES * index / max(len(frames) - 1, 1))
        buffer = io.BytesIO()
        fig.savefig(buffer, dpi=GIF_DPI, format="png")
        plt.close(fig)
        images.append(Image.open(buffer).convert("RGB").quantize(colors=64))
    images[0].save(output, save_all=True, append_images=images[1:], duration=FRAME_MS * GIF_STEP, loop=0,
                   optimize=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("model", type=pathlib.Path)
    parser.add_argument("clips", nargs=3, metavar="CLIP")
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as work:
        directory = pathlib.Path(work)
        pack, skeleton = model_pack(args.model, args.clips, directory)
        runs = {name: play(args.cc, pack, skeleton, args.clips, events, PLAYHEAD_END_MS, directory)
                for name, events in PLAYHEAD_RUNS.items()}
        blend = play(args.cc, pack, skeleton, args.clips, BLEND_SCRIPT, BLEND_END_MS, directory)
        nlerp = run_probe(args.cc, PROBE_SOURCES, "--nlerp", NLERP_DEGREES, NLERP_STEPS)
    playhead_figure(runs, args.out / "anim-playhead.png")
    blend_figure(blend, args.out / "anim-blend.png")
    nlerp_figure(nlerp, args.out / "anim-nlerp.png")
    blend_gif(blend, args.out / "anim-blend.gif")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
