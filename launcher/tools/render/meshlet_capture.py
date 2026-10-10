"""Prepare one incremental scratch tree and install meshlet-size bakes for suite captures."""
import argparse
import json
import pathlib
import shlex
import shutil
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from r3d import rebake
from r3d.import_settings import load_scene
from r3d.scene_asset import scene_id
from generated_blocks import tracked_files

DEFAULT_WORK = ROOT / "launcher/tools/results/meshlet-sizes"


def rebake_scene(scene_path, output, size):
    output = pathlib.Path(output)
    output.mkdir(parents=True, exist_ok=True)
    paths = []
    for job in load_scene(scene_path).renderers:
        target = output / job.asset_path.name
        if size == job.settings.meshlet_triangles:
            shutil.copyfile(job.asset_path, target)
        else:
            rebake.main([str(job.asset_path), "--out-dir", str(output), "--import", str(job.settings.path),
                         "--meshlet-triangles", str(size)])
        paths.append(target)
    return paths


def scratch_tree(work):
    tree = work / "tree"
    if tree.exists():
        raise ValueError(f"{tree} already exists; use a fresh --work directory")
    for name in tracked_files(ROOT):
        source, target = ROOT / name, tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target, ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
        else:
            shutil.copyfile(source, target)
    return tree


def install_row(work, metadata, size):
    if size not in metadata["sizes"]:
        raise ValueError("row size was not prepared")
    for name in metadata["meshes"]:
        shutil.copyfile(work / "bakes" / str(size) / pathlib.Path(name).name, work / "tree" / name)
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
