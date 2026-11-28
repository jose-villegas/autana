"""Render the albedo example of the scene's fitted renderer OBJECT beside its committed render, using the
CPU stage's host and its render of OBJECT."""
import copy
import os
from pathlib import Path
import subprocess
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from r3d.bake_fidelity import write_assets
from r3d.import_settings import load_scene
from r3d.lit_mesh import write_lit_mesh
from r3d.mesh_bundle import start_file
from r3d.mesh_import import bake_geometry
from r3d.scene_name import scene_id


def main(scene_path, object_name, host, work, out, frames, dt):
    root = Path.cwd()
    scene = load_scene(scene_path)
    job = copy.deepcopy(next(item for item in scene.renderers if item.object.name == object_name))
    job.bake = None
    scratch = work / "albedo"
    scratch.mkdir(exist_ok=True)
    geometry = bake_geometry(job, scene)
    write_lit_mesh(scratch, job.asset_name, geometry.positions, geometry.rgb, geometry.tris,
                   geometry.tri_double, **geometry.scale)
    assets = write_assets(job.asset_name, start_file(job, scratch), scratch, scene_path)
    subprocess.run([str(host), "--quarter", "0", "--scene", scene_id(scene_path), "--object", object_name,
                    "--frames", str(frames), "--dt", str(dt), "-o", str(scratch / "last.bmp")], check=True,
                   env={**os.environ, "AUTANA_ASSET_DIR": str(assets)})
    compare = root / "launcher/tools/render/render_compare.py"
    subprocess.run([sys.executable, str(compare), "--out", str(out / "import-light.png"),
                    "--label-a", "albedo", "--label-b", "fitted", "--row", "albedo | fitted",
                    str(scratch / "last.bmp"), str(work / "committed-smooth.bmp")], check=True)


if __name__ == "__main__":
    main(Path(sys.argv[1]).resolve(), sys.argv[2], Path(sys.argv[3]).resolve(), Path(sys.argv[4]).resolve(),
         Path(sys.argv[5]).resolve(), int(sys.argv[6]), int(sys.argv[7]))
