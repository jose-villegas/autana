"""Render albedo and face-sampling examples of the fitted variants using the CPU
stage's host and bakes: OBJECT is the smooth fit, FLAT the flat one."""
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
from r3d.scene_name import scene_id


def main(scene_path, object_name, flat_name, host, work, out, frames, dt):
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
    subprocess.run([str(host), "--quarter", "0", "--scene", scene_id(scene_path), "--object", object_name,
                    "--frames", str(frames), "--dt", str(dt), "-o", str(scratch / "last.bmp")], check=True,
                   env={**os.environ, "AUTANA_ASSET_DIR": str(assets)})
    compare = root / "launcher/tools/render/render_compare.py"
    subprocess.run([sys.executable, str(compare), "--out", str(out / "import-light.png"),
                    "--label-a", "albedo", "--label-b", "fitted", "--row", "albedo | fitted",
                    str(scratch / "last.bmp"), str(work / f"variant-{object_name}.bmp")], check=True)
    subprocess.run([sys.executable, str(compare), "--out", str(out / "import-face-samples.png"),
                    "--label-a", "fixed 1", "--label-b", "adaptive", "--row", "fixed 1 | adaptive",
                    str(work / "face-samples/fixed1/last.bmp"), str(work / "face-samples/declared/last.bmp"),
                    "--row", "adaptive | fitted", str(work / "face-samples/declared/last.bmp"),
                    str(work / f"variant-{flat_name}.bmp")], check=True)


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), sys.argv[2], sys.argv[3], Path(sys.argv[4]).resolve(), Path(sys.argv[5]).resolve(),
         Path(sys.argv[6]).resolve(), int(sys.argv[7]), int(sys.argv[8]))
