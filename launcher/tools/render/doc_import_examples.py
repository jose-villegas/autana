"""Render albedo and face-sampling examples using the CPU stage's host and bakes."""
import copy
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from r3d.bake_fidelity import write_assets
from r3d.import_settings import load_scene
from r3d.lit_mesh import write_lit_mesh
from r3d.mesh_import import bake_geometry


def main(scene_path, object_name, work, out):
    root = Path.cwd()
    scene = load_scene(scene_path)
    job = copy.deepcopy(next(item for item in scene.renderers if item.object.name == object_name))
    job.bake = None
    scratch = work / "albedo"
    scratch.mkdir(exist_ok=True)
    geometry = bake_geometry(job, scene)
    write_lit_mesh(scratch, job.renderer.variant.name, geometry.positions, geometry.rgb, geometry.tris,
                   geometry.tri_double, **geometry.scale)
    assets = write_assets(job.asset_name, scratch / f"{job.renderer.variant.name}.mesh", scratch, scene_path)
    host = work / ("scene_viewer_render.exe" if os.name == "nt" else "scene_viewer_render")
    subprocess.run([str(host), "--quarter", "0", "--scene", scene_path.name.removesuffix(".scene.toml"), "--object", object_name,
                    "--frames", "5", "--dt", "5000", "-o", str(scratch / "last.bmp")], check=True,
                   env={**os.environ, "AUTANA_ASSET_DIR": str(assets)})
    compare = root / "launcher/tools/render/render_compare.py"
    subprocess.run([sys.executable, str(compare), "--out", str(out / "import-light.png"),
                    "--label-a", "albedo", "--label-b", "baked light", "--row", "albedo | baked light",
                    str(scratch / "last.bmp"), str(work / "committed-smooth.bmp")], check=True)
    subprocess.run([sys.executable, str(compare), "--out", str(out / "import-face-samples.png"),
                    "--label-a", "fixed 1", "--label-b", "adaptive", "--row", "fixed 1 | adaptive",
                    str(work / "sampling/fixed1/last.bmp"), str(work / "sampling/declared/last.bmp")], check=True)


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), sys.argv[2], Path(sys.argv[3]).resolve(), Path(sys.argv[4]).resolve())
