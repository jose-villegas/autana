"""Mesh-Import.md's probe figure for Sponza's atrium fit and Bistro's street fit (r3d/views/probe_figures.py).

    python launcher/demo/probe_figures.py --out DIR
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools"))
from docs.producer import Producer  # noqa: E402
from r3d.views import probe_figures  # noqa: E402

PRODUCER = Producer(area="render", needs=("host",),
                    run=("--out", "{out}"),
                    reads=("launcher/demo/sponza/sponza.scene.toml", "launcher/demo/sponza/sponza.import.toml",
                           "launcher/demo/sponza/source/*.obj", "launcher/demo/sponza/source/*.mtl",
                           "launcher/demo/sponza/source/textures/*.png",
                           "launcher/demo/bistro/bistro.scene.toml", "launcher/demo/bistro/bistro.import.toml",
                           "launcher/demo/bistro/source/*.obj", "launcher/demo/bistro/source/*.mtl",
                           "launcher/demo/bistro/source/textures/*.png",
                           "launcher/demo/bistro/street.anim.toml", "launcher/demo/bistro/street.keys.toml"),
                    outputs=("sponza-atrium_fitted_full-probes-orbit.gif", "sponza-atrium_fitted_full-probes-1.png",
                             "sponza-atrium_fitted_full-probes-2.png", "sponza-atrium_fitted_full-probes-3.png",
                             "sponza-atrium_fitted_full-probes-4.png", "bistro-street_fitted_full-probes-orbit.gif",
                             "bistro-street_fitted_full-probes-1.png", "bistro-street_fitted_full-probes-2.png",
                             "bistro-street_fitted_full-probes-3.png", "bistro-street_fitted_full-probes-4.png",
                             "probe-fates.png"))
SCENES = ("--scene", "launcher/demo/sponza/sponza.scene.toml", "atrium_fitted_full",
          "--scene", "launcher/demo/bistro/bistro.scene.toml", "street_fitted_full")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--out", required=True)
    return probe_figures.main([*SCENES, "--out", parser.parse_args().out])


if __name__ == "__main__":
    raise SystemExit(main())
