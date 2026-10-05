#!/usr/bin/env python3
"""Write the asset bundles: one pack per root asset, named after it.

    python launcher/tools/r3d/build_pack.py -o DIR [--image FILE] [PATH ...] [--replace NAME=FILE ...]
    python launcher/tools/r3d/build_pack.py --bundle-of ID [PATH ...]

Each PATH is an .import.toml, a .scene.toml, an .anim.toml or a folder
searched for all three; with none, launcher/main is searched. A root is a file
nothing else names: NAME.scene.toml is bundle NAME, holding the scene entry
NAME, every mesh its renderers name and the clip its camera flies; an
NAME.import.toml no scene places is bundle NAME, holding its variants; an
NAME.anim.toml no scene names is bundle NAME, holding its one clip. A mesh
is the entry <id>.mesh that mesh_import.py wrote, and its pack id is that id;
a scene (r3d/scene_asset.py) and a clip (anim/tracks_asset.py) are baked here
from their files, each with its stem for id. Ids are unique within a bundle,
whatever their type. Each bundle is written to DIR/<name>.apak; --image also
writes the partition image, the bundle directory and every bundle.
--bundle-of prints the bundle that holds entry ID. Run from the repository
root; standard library only, and no mesh is baked. Bundles are build
products, never committed.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import tracks_asset  # noqa: E402
from asset.asset_pack import PackError, build_directory, build_pack, parse_directory, parse_pack  # noqa: E402
from r3d import scene_asset  # noqa: E402
from r3d.import_settings import SettingsError, load_import_settings, load_scene  # noqa: E402
from r3d.mesh_asset import TYPE as LIT_MESH  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_SEARCH = REPO / "launcher" / "main"
SCENE, IMPORT, CLIP = ".scene.toml", ".import.toml", tracks_asset.SUFFIX
BUNDLE_SUFFIX = ".apak"


def input_files(paths):
    """The import, scene and clip files under `paths`, each folder searched."""
    found = []
    for path in map(pathlib.Path, paths):
        if path.is_dir():
            found += sorted(p for p in path.rglob("*.toml") if p.name.endswith((IMPORT, SCENE, CLIP)))
        elif path.name.endswith((IMPORT, SCENE, CLIP)):
            found.append(path)
        else:
            raise SettingsError(f"{path}: not an .import.toml, a .scene.toml, an .anim.toml or a folder")
    return found


def add_entry(entries, key, source, root):
    """Adds entry `key` from `source`; ids are unique within a bundle, whatever their type."""
    if entries.setdefault(key, source) != source:
        raise SettingsError(f"{root}: {entries[key]} and {source} both make entry {key!r}: "
                            "ids are unique within a bundle, so rename one")


def scene_entries(path, scene):
    """{entry id: its source} of a scene's bundle: its entry, its meshes and its camera's clip."""
    entries = {}
    add_entry(entries, scene_asset.scene_id(path), path, path)
    for item in scene.renderers:
        add_entry(entries, item.asset_name, item.asset_path, path)
    clip = scene.camera.component.path if scene.camera else None
    if clip:
        add_entry(entries, clip.clip, clip.animation, path)
    return entries


def bundle_files(paths):
    """{bundle name: {entry id: its source}}, one bundle per root under `paths`;
    a mesh's source is its .mesh file, a scene's its .scene.toml, a clip's
    its .anim.toml."""
    files = input_files(paths)
    roots, placed = {}, set()
    for path in files:
        if path.name.endswith(SCENE):
            scene = load_scene(path)
            placed.update(item.settings.path for item in scene.renderers)
            roots[path] = scene_entries(path, scene)
            placed.update(source.resolve() for source in roots[path].values() if source.name.endswith(CLIP))
    for path in files:
        if path.name.endswith(IMPORT) and path.resolve() not in placed:
            settings = load_import_settings(path)
            roots[path] = {v.name: settings.mesh_dir / f"{v.name}.mesh" for v in settings.variants}
    for path in files:
        if path.name.endswith(CLIP) and path.resolve() not in placed:
            roots[path] = {tracks_asset.clip_id(path): path}
    bundles, owner = {}, {}
    for path, meshes in roots.items():
        name = path.name.removesuffix(SCENE).removesuffix(IMPORT).removesuffix(CLIP)
        if name in bundles:
            raise SettingsError(f"two roots make a bundle named {name!r}: {owner[name]} and {path}")
        owner[name] = path
        bundles[name] = meshes
    holder = {}
    for name, meshes in bundles.items():
        for mesh in meshes:
            if mesh in holder:
                raise SettingsError(f"{mesh!r} is named by bundles {holder[mesh]!r} and {name!r}: "
                                    "a shared asset needs a bundle of its own, which is not built yet")
            holder[mesh] = name
    return bundles


def bundle_bytes(paths, replace=()):
    """{bundle name: its pack's bytes}; each --replace NAME=FILE takes mesh NAME from FILE."""
    bundles = bundle_files(paths)
    for item in replace:
        mesh, _, file = item.partition("=")
        holder = next((meshes for meshes in bundles.values() if mesh in meshes), None)
        if holder is None:
            raise SettingsError(f"--replace {mesh}: no such mesh")
        holder[mesh] = pathlib.Path(file)
    missing = [str(source) for sources in bundles.values() for source in sources.values() if not source.is_file()]
    if missing:
        raise SettingsError("no baked mesh at " + ", ".join(missing) + "; run mesh_import.py first")
    return {name: build_pack([pack_entry(key, source) for key, source in sorted(sources.items())])
            for name, sources in sorted(bundles.items())}


def pack_entry(key, source):
    """The pack entry `key` from its source: a scene or a clip baked from its
    file, else a mesh's bytes."""
    if source.name.endswith(SCENE):
        return key, scene_asset.TYPE, scene_asset.bake(source)
    if source.name.endswith(CLIP):
        return key, tracks_asset.TYPE, tracks_asset.bake(source)
    return key, LIT_MESH, source.read_bytes()


def write_if_changed(path, data):
    if not path.is_file() or path.read_bytes() != data:
        path.write_bytes(data)


def write_bundles(out, packs, image=None):
    """Writes each pack to out/<name>.apak, removing any other .apak there, and
    with `image` the partition image."""
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("*" + BUNDLE_SUFFIX):
        if stale.name.removesuffix(BUNDLE_SUFFIX) not in packs:
            stale.unlink()
    for name, pack in packs.items():
        write_if_changed(out / f"{name}{BUNDLE_SUFFIX}", pack)
    if image is not None:
        data = build_directory(sorted(packs.items()))
        parse_directory(data)
        image.parent.mkdir(parents=True, exist_ok=True)
        write_if_changed(image, data)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", help="import files, scene files or folders; launcher/main when omitted")
    parser.add_argument("-o", "--out", help="the folder the bundles are written to")
    parser.add_argument("--image", help="also write the partition image: the bundle directory and every bundle")
    parser.add_argument("--replace", action="append", default=[], metavar="NAME=FILE",
                        help="take mesh NAME from FILE: a scratch bake beside the committed ones")
    parser.add_argument("--bundle-of", metavar="ID", help="print the bundle that holds entry ID and write nothing")
    args = parser.parse_args(argv)
    paths = args.paths or [DEFAULT_SEARCH]
    try:
        if args.bundle_of:
            holder = [name for name, meshes in bundle_files(paths).items() if args.bundle_of in meshes]
            if not holder:
                parser.error(f"no bundle holds entry {args.bundle_of!r}")
            print(holder[0])
            return 0
        if not args.out:
            parser.error("-o DIR is required")
        packs = bundle_bytes(paths, args.replace)
        contents = {name: parse_pack(pack) for name, pack in packs.items()}
        write_bundles(pathlib.Path(args.out), packs, pathlib.Path(args.image) if args.image else None)
    except (SettingsError, PackError, tracks_asset.TracksError, scene_asset.SceneError) as error:
        parser.error(str(error))
    for name, entries in contents.items():
        print(f"wrote {name}{BUNDLE_SUFFIX} ({len(packs[name])} bytes): " + ", ".join(sorted(entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
