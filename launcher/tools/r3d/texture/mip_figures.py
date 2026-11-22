#!/usr/bin/env python3
"""Texture-Mapping.md's mip-level figure.

    mip_figures.py --cc CC --out DIR --textures TOML --source PNG --title TEXT

Top: the source texture's mip chain as the LTEX baker makes it (texture_asset.py), level 0 at the size the
textures file's first renderer gives it, each level at its true size beside the last. Below: a receding floor drawn
by the span fill (mip_figures_probe.c) with a texture whose every level is one solid colour, so each pixel shows the
level its triangle picked; beside it, the pixels whose triangle picked a finer level than their own step reads, the
far part of each triangle, where it aliases.
"""
import argparse
import pathlib
import sys
import tomllib

import numpy as np

TOOLS = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "r3d"))
from docs.figures import run_probe  # noqa: E402
from docs.plot_style import FAINT, LABEL_SIZE, MUTED, SLOTS, save, subplots  # noqa: E402
from matplotlib.patches import Patch  # noqa: E402
from obj import Texture  # noqa: E402
from texture import texture_asset  # noqa: E402

ROOT = TOOLS.parents[1]
PROBE = pathlib.Path(__file__).with_name("mip_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/main/render/texture/r3d_span_textured.c",
           ROOT / "launcher/main/render/texture/r3d_texture.c", ROOT / "launcher/main/render/r3d_span.c",
           ROOT / "launcher/main/render/path/r3d_path.c", ROOT / "launcher/main/asset/asset_pack.c",
           ROOT / "launcher/main/render/r3d_lit_mesh.c")
IMAGE = "texture-mip-levels.png"
NONE = -1
CHAIN_GAP = 10  # level-0 texels between two levels of the chain
FIGURE_SIZE = (9.0, 6.4)
CHAIN_HEIGHT_SHARE = 0.9  # the chain row's height against a floor row's
LABEL_DROP = 3  # level-0 texels between the chain and a level's label
LABEL_ROW = 7  # level-0 texels between the two rows of the small levels' labels
CROWDED = 4  # a level this many times narrower than level 0 has its label staggered
ALIASED = SLOTS[7]


def chain_image(indexed):
    """Every level of `indexed` as display RGB, side by side and bottom-aligned, on a transparent strip."""
    tint = np.array(indexed.tint, dtype=np.float64) / texture_asset.BYTE_MAX
    palette = np.array(indexed.palette, dtype=np.float64) / texture_asset.ONE * tint
    sides = [len(level) for level in indexed.levels]
    width = sum(sides) + CHAIN_GAP * (len(sides) - 1)
    strip = np.zeros((sides[0], width, 4))
    x = 0
    for side, level in zip(sides, indexed.levels):
        strip[sides[0] - side:, x:x + side, :3] = np.clip(palette[np.array(level)], 0.0, 1.0)
        strip[sides[0] - side:, x:x + side, 3] = 1.0
        x += side + CHAIN_GAP
    return strip, sides


def level_image(levels, count):
    """Each level's colour where a level is given, the page where none."""
    colours = np.array([[int(c[i:i + 2], 16) / 255 for i in (1, 3, 5)] for c in SLOTS[:count]])
    image = np.ones((*levels.shape, 3))
    drawn = levels != NONE
    image[drawn] = colours[levels[drawn]]
    return image


def baked_texture(textures, source):
    """`source` as the bake packs it at the size `textures` gives: an LTEX entry, decoded back."""
    size = tomllib.loads(textures.read_text(encoding="utf-8"))["renderers"][0]["size"]
    indexed = texture_asset.material_texture(Texture(source).levels[0], size)
    return texture_asset.decode(texture_asset.pack_ltex(indexed))


def mip_figure(data, args):
    shape = (data["height"], data["width"])
    picked = np.array(data["picked"]).reshape(shape)
    ideal = np.array(data["ideal"]).reshape(shape)
    count = data["levels"]
    strip, sides = chain_image(baked_texture(args.textures, args.source))
    fig, axes = subplots(2, 2, FIGURE_SIZE, height_ratios=(CHAIN_HEIGHT_SHARE, 1))
    gs = axes[0, 0].get_gridspec()
    for axis in axes[0]:
        axis.remove()
    chain = fig.add_subplot(gs[0, :])
    chain.imshow(strip, interpolation="nearest")
    x = 0
    for level, side in enumerate(sides):
        # The small levels' labels alternate between two rows, clear of their neighbours.
        below = LABEL_DROP + (LABEL_ROW * (level % 2) if side < sides[0] // CROWDED else 0)
        chain.text(x + side / 2, sides[0] + below, f"level {level}: {side}x{side}", ha="center", va="top",
                   fontsize=LABEL_SIZE, color=MUTED)
        x += side + CHAIN_GAP
    chain.set_title(args.title)
    chain.axis("off")
    axes[1, 0].imshow(level_image(picked, count), interpolation="nearest")
    axes[1, 0].set_title("The level each triangle picked")
    aliased = (picked != NONE) & (picked < ideal)
    shown = level_image(picked, count) * 0.35 + 0.65
    shown[aliased] = [int(ALIASED[i:i + 2], 16) / 255 for i in (1, 3, 5)]
    axes[1, 1].imshow(shown, interpolation="nearest")
    share = aliased.sum() / max(1, (picked != NONE).sum())
    axes[1, 1].set_title(f"Finer than the pixel's own step: {share:.0%} of the floor")
    for axis in axes[1]:
        axis.set_xticks([])
        axis.set_yticks([])
        for spine in axis.spines.values():
            spine.set_color(FAINT)
    used = sorted(set(np.unique(picked)) - {NONE})
    handles = [Patch(color=SLOTS[level], label=f"level {level}") for level in used]
    handles.append(Patch(color=ALIASED, label="aliases"))
    fig.legend(handles=handles, loc="lower center", ncol=len(handles), fontsize=LABEL_SIZE, frameon=False)
    fig.get_layout_engine().set(rect=(0, 0.05, 1, 0.95))
    save(fig, args.out / IMAGE)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", type=pathlib.Path, required=True)
    parser.add_argument("--textures", type=pathlib.Path, required=True, help="a textures.toml: its first size")
    parser.add_argument("--source", type=pathlib.Path, required=True, help="the texture whose chain is shown")
    parser.add_argument("--title", required=True, help="the chain's title")
    args = parser.parse_args(argv)
    args.out.mkdir(parents=True, exist_ok=True)
    mip_figure(run_probe(args.cc, SOURCES), args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
