"""Levels-of-Detail.md's LOD blend clip and still in this app's fitted Sponza atrium: a camera closing in on
the lion relief at a coarse cut, drawn by r3d/lod/blend_render_figures.py.

    python launcher/main/apps/render_lab/tools/lod_blend_pop.py --script ... --render "..." --morph MESH --out DIR
"""
import pathlib
import sys

TOOLS = pathlib.Path(__file__).resolve().parents[4] / "tools"
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(TOOLS / "r3d" / "lod"))
from blend_render_figures import main  # noqa: E402
from docs.producer import Producer  # noqa: E402

PRODUCER = Producer(area="render", needs=("host", "bakes"),
                    run=("--script", "launcher/main/apps/render_lab/tools/render_lab_render_host.sh",
                         "--render", "--scene sponza-fitted-full --camera lion --no-hud --frames 303 --dt 33"
                                     " --set render.lod.tolerance_px=16",
                         "--morph", "sponza.atrium_fitted_full", "--out", "{out}"),
                    outputs=("lod-blend-pop.gif", "lod-blend-pop.png"))

if __name__ == "__main__":
    main()
