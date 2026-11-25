"""LTEX.md's sheets of Bistro's street with and without its textures, Texture-Mapping.md's of its near floor at each
perspective divide spacing and of its high view with and without its flat levels drawn untextured
(r3d/texture/texture_figures.py), and its cobblestone's mip chain over the span fill's level picks
(r3d/texture/mip_figures.py).

    python launcher/demo/bistro/bistro_figures.py --cc CC --out DIR
"""
import argparse
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "r3d" / "texture"))
import mip_figures  # noqa: E402
import texture_figures  # noqa: E402
from docs.producer import Producer  # noqa: E402

PRODUCER = Producer(area="render", needs=("host", "bakes"), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("bistro-street-textured.png", "bistro-street-textured.crops.png",
                             "bistro-street-divide.png", "bistro-street-far-levels.png",
                             "bistro-street-far-levels.crops.png", "texture-mip-levels.png"))
STREET = ("--scene", "launcher/demo/bistro/bistro.scene.toml", "--object", "street_fitted_full",
          "--reference", "bistro.street_fitted_full.reference.tar.xz", "--frame", "10", "--dt", "1000",
          "--title", "Bistro street, fitted full: cobblestones and beige brick textured",
          "--image", "bistro-street-textured.png", "--divides", "1,0,16,8", "--divide-image", "bistro-street-divide.png",
          "--far-frame", "47", "--far-image", "bistro-street-far-levels.png",
          "--far-title", "Bistro street from above: every level textured, then each texture's flat levels untextured")
MIP = ("--textures", "launcher/demo/bistro/bistro.textures.toml",
       "--source", "launcher/demo/bistro/source/textures/pavement_cobblestone_02_basecolor.png",
       "--title", "A Bistro cobblestone's mip chain, each level half the last, at true size")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    texture_figures.main([*STREET, "--out", args.out])
    return mip_figures.main([*MIP, "--cc", args.cc, "--out", args.out])


if __name__ == "__main__":
    raise SystemExit(main())
