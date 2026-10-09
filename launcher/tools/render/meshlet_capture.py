"""Prepare meshlet-size asset variants in one scratch firmware tree.

Only prepare and select build firmware; all board commands are printed for the
capturing session. Stamp checks its before/after BUILD_ID records and records
the selected pack hash in the monitor capture.
"""
import argparse
import hashlib
import json
import pathlib
import re
import shlex
import shutil
import subprocess
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
        raise ValueError(f"{tree} already exists; use select for prepared variants")
    for name in tracked_files(ROOT):
        source, target = ROOT / name, tree / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, target, ignore=shutil.ignore_patterns(".git", "build", "__pycache__"))
        else:
            shutil.copyfile(source, target)
    return tree


def select(work, row):
    metadata = json.loads((work / "manifest.json").read_text())
    if not row or (row != "cull-off" and not row.isdigit()):
        raise ValueError("row must be cull-off or a prepared size")
    size = 32 if row == "cull-off" else int(row)
    if size not in metadata["sizes"]:
        raise ValueError(f"size {size} was not prepared")
    tree = work / "tree"
    for name in metadata["meshes"]:
        shutil.copyfile(work / "bakes" / str(size) / pathlib.Path(name).name, tree / name)
    subprocess.run(["bash", str(ROOT / "tools/autana"), "--project", str(tree), "build", "dev"],
                   cwd=ROOT, check=True)
    build = tree / "launcher/build.dev"
    build_id = (build / "build_id.txt").read_text().strip()
    pack = build / "assets" / (metadata["pack"] + ".apak")
    digest = hashlib.sha256(pack.read_bytes()).hexdigest()
    previous = metadata.get("build_id")
    if previous and previous != build_id:
        raise ValueError("size variants changed the firmware build id")
    metadata["build_id"] = build_id
    metadata.setdefault("packs", {})[str(size)] = digest
    metadata["selected"] = row
    (work / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
    snapshot = work / "packs" / f"{size}.apak"
    snapshot.parent.mkdir(exist_ok=True)
    shutil.copyfile(pack, snapshot)
    return metadata


def commands(work, metadata):
    quote = lambda path: shlex.quote(pathlib.Path(path).as_posix())
    tool = quote(ROOT / "launcher/tools/render/meshlet_capture.py")
    for row in ("cull-off", *(str(size) for size in metadata["sizes"])):
        capture = ROOT / f"docs/render/data/meshlets-{row}-board.log"
        before, after = work / f"{row}-before.txt", work / f"{row}-after.txt"
        print(f"python {tool} select --work {quote(work)} --row {row}")
        print(f"autana --wait 3600 --project {quote(work / 'tree')} flash dev")
        if metadata.get("open_app"):
            print("autana open " + shlex.quote(metadata["open_app"]))
        if metadata.get("scene_command"):
            print("autana " + shlex.join(metadata["scene_command"]))
        print("autana status")
        print(f"autana buildid > {quote(before)}")
        print(f"autana tune render.cull {int(row != 'cull-off')}")
        print(f"autana monitor 30 --out {quote(capture)}")
        print("autana status")
        print(f"autana buildid > {quote(after)}")
        print(f"python {tool} stamp --work {quote(work)} --row {row} --capture {quote(capture)} "
              f"--before {quote(before)} --after {quote(after)}")


def stamp(work, row, capture, before, after):
    metadata = json.loads((work / "manifest.json").read_text())
    if metadata["selected"] != row:
        raise ValueError("selected pack differs from capture row")
    def identity(path):
        found = re.findall(r"BUILD_ID[= ]([0-9a-f]{12}-dev)", path.read_text())
        if len(found) != 1 or found[0] != metadata["build_id"]:
            raise ValueError(f"{path}: missing or mismatched build id")
        return found[0]
    build_id = identity(before)
    identity(after)
    size = 32 if row == "cull-off" else int(row)
    pack = work / "tree/launcher/build.dev/assets" / (metadata["pack"] + ".apak")
    digest = hashlib.sha256(pack.read_bytes()).hexdigest()
    if digest != metadata["packs"][str(size)]:
        raise ValueError("built pack differs from prepared snapshot")
    raw = capture.read_bytes()
    if b"MESHLET_CAPTURE" in raw:
        raise ValueError("capture already stamped")
    header = f"MESHLET_CAPTURE build_id={build_id} pack_sha256={digest} size={size} cull={int(row != 'cull-off')}\n"
    capture.write_bytes(header.encode() + raw)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("prepare", "select", "stamp", "commands"))
    parser.add_argument("--scene", type=pathlib.Path)
    parser.add_argument("--sizes", type=int, choices=(16, 32, 64), nargs="+", default=[16, 32, 64])
    parser.add_argument("--work", type=pathlib.Path, default=DEFAULT_WORK)
    parser.add_argument("--row")
    parser.add_argument("--open-app", help="app name for the printed capture commands")
    parser.add_argument("--scene-command", nargs="+", help="console command selecting the shipped scene")
    for name in ("capture", "before", "after"):
        parser.add_argument("--" + name, type=pathlib.Path)
    args = parser.parse_args(argv)
    work = args.work.resolve()
    try:
        if not work.is_relative_to(ROOT / "launcher/tools/results"):
            raise ValueError("scratch work must be inside launcher/tools/results")
        if args.command == "prepare":
            if args.scene is None or 32 not in args.sizes or len(set(args.sizes)) != len(args.sizes):
                raise ValueError("prepare needs --scene and unique sizes including 32")
            scene_path = args.scene.resolve()
            scene = load_scene(scene_path)
            work.mkdir(parents=True, exist_ok=True)
            scratch_tree(work)
            metadata = dict(scene=str(scene_path.relative_to(ROOT)), pack=scene_id(scene_path), sizes=args.sizes,
                            open_app=args.open_app, scene_command=args.scene_command,
                            meshes=[str(job.asset_path.relative_to(ROOT)) for job in scene.renderers])
            (work / "manifest.json").write_text(json.dumps(metadata, indent=2) + "\n")
            for size in args.sizes:
                rebake_scene(scene_path, work / "bakes" / str(size), size)
                metadata = select(work, str(size))
            commands(work, metadata)
        elif args.command == "select":
            select(work, args.row)
        elif args.command == "stamp":
            if not all((args.row, args.capture, args.before, args.after)):
                raise ValueError("stamp needs --row, --capture, --before and --after")
            stamp(work, args.row, args.capture, args.before, args.after)
        else:
            commands(work, json.loads((work / "manifest.json").read_text()))
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        return 2

if __name__ == "__main__":
    sys.exit(main())
