#!/usr/bin/env python3
"""Texture-Mapping.md's mip-level figure from Bistro's cobblestone, drawn by r3d/texture/mip_figures.py.

    python launcher/demo/bistro/bistro_mip_figures.py --cc CC --out DIR --textures ... --source ... --title ...
"""
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "r3d" / "texture"))
from docs.producer import Producer  # noqa: E402
from mip_figures import main  # noqa: E402

PRODUCER = Producer(area="render", needs=("host", "bakes"),
                    run=("--cc", "{cc}", "--out", "{out}", "--textures", "launcher/demo/bistro/bistro.textures.toml",
                         "--source", "launcher/demo/bistro/source/textures/pavement_cobblestone_02_basecolor.png",
                         "--title", "A Bistro cobblestone's mip chain, each level half the last, at true size"),
                    outputs=("texture-mip-levels.png",))

if __name__ == "__main__":
    raise SystemExit(main())
