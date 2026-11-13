"""The update-rate figure of docs/render/animation/Skinned-Meshes.md: how far a skinned copy posed every Nth frame
lags its true pose, in pixels, at the largest size its tier allows. The tiers come from the render settings
(default.render.toml's anim table); the poses from the glTF reference skinner, over one loop of each clip.

    python launcher/tools/anim/playback/rate_figures.py MODEL.glb CLIP [CLIP ...] --out PNG

Between poses a copy draws the pose of the last frame it was posed on, which caught up its owed time, so it
lags by the clip's motion over up to N - 1 frames. The worst vertex's offset, in model units, is scaled to
pixels as the LOD measures size: the bind pose's box, its half diagonal the radius, drawn at the tier's widest
(its threshold plus the hysteresis). Prints each clip's worst and mean per tier.
"""

import argparse
import math
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
from build.c_header import read_define  # noqa: E402
from docs.plot_style import FAINT, LABEL_SIZE, MUTED, SLOTS, save, style_axes, subplots  # noqa: E402
from r3d import gltf_skin  # noqa: E402
from render.settings import render_asset  # noqa: E402

# The board still-herd comparison uses this same step.
FRAME_MS = read_define(TOOLS.parent / "main/lab/test/lab_test.h", "LAB_TEST_FRAME_MS")
MS_PER_S = 1000
PANEL = (3.2, 2.6)  # inches per panel
ONE_PIXEL = 1.0
TIER_NAMES = ("mid", "far")


def tiers(settings):
    """(label, interval, widest radius px) of each tier that skips frames, mid then far."""
    hysteresis = settings["anim.hysteresis_px"]
    out = []
    for name in TIER_NAMES:
        every, px = settings[f"anim.{name}_every"], settings[f"anim.{name}_px"]
        out.append((f"{name}: every {every}, {px + hysteresis:g} px", every, px + hysteresis))
    return out


def bind_radius(positions):
    """The half diagonal of the positions' box: the radius r3d_lod_bounds_pixels() projects."""
    lo = [min(p[i] for p in positions) for i in range(3)]
    hi = [max(p[i] for p in positions) for i in range(3)]
    return math.dist(lo, hi) / 2


def loop(asset, clip):
    """The frames' times in seconds over one loop of `clip`, and the skinned positions at each."""
    count = max(1, int(asset.duration(clip) * MS_PER_S // FRAME_MS))
    times = [i * FRAME_MS / MS_PER_S for i in range(count)]
    return times, [asset.skin(asset.sample(clip, t))[0] for t in times]


def lag(frames, every):
    """The worst vertex's offset from its true pose at each frame, for a copy posed on every `every`th frame."""
    return [max(math.dist(a, b) for a, b in zip(posed, frames[i - i % every])) for i, posed in enumerate(frames)]


def figure(rows, clips, output):
    fig, axes = subplots(1, len(clips), (PANEL[0] * len(clips), PANEL[1]), sharey=True, squeeze=False)
    axes = axes[0]
    for axis, clip in zip(axes, clips):
        style_axes(axis, clip)
        for colour, (label, times, pixels) in zip(SLOTS, rows[clip]):
            axis.step(times, pixels, where="post", color=colour, label=label)
        axis.axhline(ONE_PIXEL, color=FAINT, linestyle="--", linewidth=1)
        axis.set_xlabel("clip time, s", fontsize=LABEL_SIZE)
    axes[0].set_ylabel("worst vertex behind its pose, px", fontsize=LABEL_SIZE, color=MUTED)
    axes[0].legend(fontsize=LABEL_SIZE, loc="upper left")
    save(fig, output)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("model", type=pathlib.Path)
    parser.add_argument("clips", nargs="+")
    parser.add_argument("--out", type=pathlib.Path, required=True)
    args = parser.parse_args()
    asset = gltf_skin.SkinnedAsset(*gltf_skin.load_asset(args.model))
    radius = bind_radius(asset.skin(asset.sample(None, 0))[0])
    settings = render_asset.read(render_asset.DEFAULT)
    rows = {}
    for clip in args.clips:
        times, frames = loop(asset, clip)
        rows[clip] = []
        for label, every, px in tiers(settings):
            pixels = [d / radius * px for d in lag(frames, every)]
            rows[clip].append((label, times, pixels))
            print(f"{clip} {label}: worst {max(pixels):.2f} px, mean {sum(pixels) / len(pixels):.2f} px")
    figure(rows, args.clips, args.out)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
