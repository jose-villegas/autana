"""Prepare one incremental scratch tree and install meshlet-size bakes for suite captures."""
import argparse
import json
import pathlib
import shlex
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from bake import bake
from r3d import rebake
from r3d.import_settings import load_scene
from r3d.scene_name import scene_id
from build.scratch import scratch_tree
from showcase import showcase

DEFAULT_WORK = ROOT / "launcher/tools/results/meshlet-sizes"


def rebake_scene(scene_path, output, size):
    """The scene's meshes from the bake cache in `output` under their entry names, its showcase re-clustered to
    `size`. The others keep the shipped size, so the suite's budgets for them hold on every row."""
    output = pathlib.Path(output)
    output.mkdir(parents=True, exist_ok=True)
    found = [item for item in bake.bakes([scene_path]) if item.packed]
    cached = {item.tree.resolve(): path for item, path in
              bake.fetch_all(found, bake.read_lock(), bake.default_cache()).items()}
    paths = []
    scene = load_scene(scene_path)
    for job in scene.renderers:
        target = output / job.asset_path.name
        shutil.copyfile(cached[job.asset_path.resolve()], target)
        if job.object.name == showcase(scene) and size != job.settings.meshlet_triangles:
            rebake.main([str(target), "--import", str(job.settings.path), "--meshlet-triangles", str(size)])
        paths.append(target)
    return paths


def install_row(work, metadata, size):
    """Points the scratch tree's lock at the row's bakes and puts them in the bake cache, which packs read."""
    if size not in metadata["sizes"]:
        raise ValueError("row size was not prepared")
    lock = work / "tree" / bake.LOCK.relative_to(ROOT)
    tables = bake.read_lock_tables(lock)
    names = {pathlib.Path(name).name for name in metadata["meshes"]}
    rows = [row for row in tables["bake"].values() if row["output"] in names]
    if {row["output"] for row in rows} != names:
        raise ValueError("the scratch lock does not hold every mesh of the scene")
    for row in rows:
        path = work / "bakes" / str(size) / row["output"]
        row.update(sha256=bake.file_sha256(path), size=path.stat().st_size)
        bake.store(path, row, bake.MESH_SUFFIX, bake.default_cache())
    bake.write_lock(tables["bake"].values(), lock, tables["poses"].values(), tables["mesh_cpu_vendor"])
    quote = lambda path: shlex.quote(pathlib.Path(path).as_posix())
    capture = ROOT / f"docs/render/data/meshlets-{size}-board.log"
    print(f"autana --wait 3600 --project {quote(work / 'tree')} suite {shlex.quote(metadata['suite'])} "
          f"--flash --out {quote(capture)}")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "row"))
    parser.add_argument("size", type=int, nargs="?", choices=(16, 32, 64))
    parser.add_argument("--scene", type=pathlib.Path)
    parser.add_argument("--sizes", type=int, choices=(16, 32, 64), nargs="+", default=[16, 32, 64])
    parser.add_argument("--work", type=pathlib.Path, default=DEFAULT_WORK)
    parser.add_argument("--suite", help="fixed-pose frame-cost suite")
    args = parser.parse_args(argv)
    work = args.work.resolve()
    try:
        if not work.is_relative_to(ROOT / "launcher/tools/results"):
            raise ValueError("scratch work must be inside launcher/tools/results")
        if args.command == "prepare":
            if args.scene is None or not args.suite or 32 not in args.sizes or len(set(args.sizes)) != len(args.sizes):
                raise ValueError("prepare needs --scene, --suite and unique sizes including 32")
            scene_path = args.scene.resolve()
            scene = load_scene(scene_path)
            work.mkdir(parents=True, exist_ok=True)
            metadata = dict(scene=str(scene_path.relative_to(ROOT)), pack=scene_id(scene_path), sizes=args.sizes,
                            suite=args.suite,
                            meshes=[str(job.asset_path.relative_to(ROOT)) for job in scene.renderers])
            (work / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
            for size in args.sizes:
                rebake_scene(scene_path, work / "bakes" / str(size), size)
            scratch_tree(work)
        else:
            if args.size is None:
                raise ValueError("row needs a size")
            install_row(work, json.loads((work / "manifest.json").read_text()), args.size)
        return 0
    except (ValueError, OSError) as error:
        print(error, file=sys.stderr)
        return 2

if __name__ == "__main__":
    sys.exit(main())
