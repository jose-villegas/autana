#!/usr/bin/env python3
"""What culling saves on the board: the scene's showcase drawn with culling
on and off along its camera path, read from the meshlet-size captures (the
shipped size's capture also measures culling off).
launcher/tools/render/render_doc_images.sh runs it.

    python launcher/tools/render/settings/cull_saving.py --scene SCENE.scene.toml --captures DIR --table MD

It writes --table: the block docs/render/Render-Settings.md shows, one row
per setting with the mean of each frame stage, then off less on: what culling saves.
"""
import argparse
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[4]
for folder in ("launcher/tools", "launcher/tools/render", "launcher/tools/perf"):
    sys.path.insert(0, str(ROOT / folder))
from doc_tables import markdown  # noqa: E402
from meshlet_sizes import capture_means  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402
from showcase import showcase  # noqa: E402

COLUMNS = ("Cull ms", "Transform ms", "Draw ms", "Frame ms")  # the order capture_means gives them in


def table(scene_path, captures):
    scene = load_scene(scene_path)
    job = next(job for job in scene.renderers if job.object.name == showcase(scene))
    means = capture_means(captures, job.object.name, str(job.settings.meshlet_triangles))
    on, off = means[str(job.settings.meshlet_triangles)], means["cull-off"]
    rows = [("`cull` on", *on), ("`cull` off", *off), ("off - on", *(b - a for a, b in zip(on, off)))]
    return markdown(["Setting", *COLUMNS], [(label, *(f"{value:.2f}" for value in values)) for label, *values in rows])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scene", required=True, type=pathlib.Path)
    parser.add_argument("--captures", required=True, type=pathlib.Path)
    parser.add_argument("--table", required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    args.table.write_text(table(args.scene, args.captures), encoding="utf-8")


if __name__ == "__main__":
    main()
