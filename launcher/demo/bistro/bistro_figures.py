"""LTEX.md's sheets of Bistro's street with and without its textures, and of its near floor at each perspective
divide spacing, drawn by r3d/texture/texture_figures.py.

    python launcher/demo/bistro/bistro_figures.py --scene ... --object ... --reference ... --out DIR
"""
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "r3d" / "texture"))
from docs.producer import Producer  # noqa: E402
from texture_figures import main  # noqa: E402

PRODUCER = Producer(area="render", needs=("host", "bakes"),
                    run=("--scene", "launcher/demo/bistro/bistro.scene.toml", "--object", "street_fitted_full",
                         "--reference", "bistro.street_fitted.reference.tar.xz", "--frame", "10", "--dt", "1000",
                         "--title", "Bistro street, fitted full: cobblestones and beige brick textured",
                         "--image", "bistro-street-textured.png", "--divides", "1,0,16,8",
                         "--divide-image", "bistro-street-divide.png", "--out", "{out}"),
                    outputs=("bistro-street-textured.png", "bistro-street-textured.crops.png",
                             "bistro-street-divide.png"))

if __name__ == "__main__":
    main()
