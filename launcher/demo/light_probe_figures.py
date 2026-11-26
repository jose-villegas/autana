"""Light-Probes.md's figures, the grid plots drawn over Sponza's baked probes (r3d/probes/light_probe_figures.py).

    python launcher/demo/light_probe_figures.py --cc CC --out DIR
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from docs.producer import Producer  # noqa: E402
from r3d.probes import light_probe_figures  # noqa: E402

SCENE = "launcher/demo/sponza/sponza.scene.toml"
PRODUCER = Producer(area="render", needs=("host", "bakes"), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("light-probe-sphere.png", "light-probe-blend.png", "light-probe-sponza.png",
                             "light-probe-sponza-spheres.png"))


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--cc", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()
    light_probe_figures.main(["--cc", args.cc, "--out", args.out, "--scene", SCENE])


if __name__ == "__main__":
    main()
