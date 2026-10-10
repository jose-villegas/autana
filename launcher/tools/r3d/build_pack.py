#!/usr/bin/env python3
"""Write the asset packs: one pack per root asset, named after it.

    python launcher/tools/r3d/build_pack.py -o DIR [--image FILE] [PATH ...] [--replace NAME=FILE ...]
    python launcher/tools/r3d/build_pack.py --pack-of ID [PATH ...]

Each PATH is an .import.toml, a .scene.toml, an .anim.toml or a folder
searched for all three; with none, launcher/main is searched. An app's
demo_assets.toml adds launcher/demo/NAME for each NAME in its `demo` list;
one inside launcher/demo is refused. A folder reached twice is searched
once. A root is a file nothing else names: NAME.scene.toml is pack NAME,
holding the scene entry NAME, every mesh its renderers name and the clip its
camera flies; an NAME.import.toml no scene places is pack NAME, holding its
variants; an NAME.anim.toml no scene names is pack NAME, holding its one clip. A mesh
is the entry <id>.mesh that mesh_import.py wrote, and its pack id is that id;
a scene (r3d/scene_asset.py) and a clip (anim/tracks_asset.py) are baked here
from their files, each with its stem for id. Ids are unique within a pack,
whatever their type. Each pack is written to DIR/<name>.apak; --image also
writes the partition image, the pack directory and every pack.
Every mesh comes from the bake cache by launcher/bakes.lock (bake/bake.py):
the user cache, or --bake-cache DIR, or AUTANA_BAKE_CACHE when a process sets
it. What the cache lacks is downloaded, so a cold cache needs the network once;
--offline never downloads, and a bake that is not available fails, naming
each. The lock is this repository's: a mesh of a scene outside it is the file
beside it.
--pack-of prints the pack that holds entry ID. Run from the repository
root; standard library only, and no mesh is baked. Packs are build
products, never committed.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import tracks_asset  # noqa: E402
from asset.asset_pack import PackError, build_directory, build_pack, parse_directory, parse_pack  # noqa: E402
from r3d import scene_asset, skin_asset  # noqa: E402
from r3d.import_settings import SettingsError, albedo_jobs, load_demo_assets, load_import_settings, load_scene  # noqa: E402
from r3d.mesh_asset import TYPE as LIT_MESH  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_SEARCH = REPO / "launcher" / "main"
DEMO = REPO / "launcher" / "demo"
SCENE, IMPORT, CLIP = ".scene.toml", ".import.toml", tracks_asset.SUFFIX
PACK_SUFFIX = ".apak"


def input_files(paths):
    """The import, scene and clip files under `paths` and app-selected demos;
    manifests inside DEMO are refused, and each folder is searched once."""
    found, searched = set(), set()
    pending = list(map(pathlib.Path, paths))
    while pending:
        path = pending.pop(0).resolve()
        if path in searched:
            continue
        searched.add(path)
        if path.is_dir():
            found.update(p for p in path.rglob("*.toml") if p.name.endswith((IMPORT, SCENE, CLIP)))
            for manifest in sorted(path.rglob("demo_assets.toml")):
                if manifest.is_relative_to(DEMO.resolve()):
                    raise SettingsError(f"{manifest}: only an app names demo assets")
                for name in load_demo_assets(manifest):
                    demo = DEMO / name
                    if not demo.is_dir():
                        raise SettingsError(f"{manifest}: no demo folder for {name!r}")
                    pending.append(demo)
        elif path.name.endswith((IMPORT, SCENE, CLIP)):
            found.add(path)
        else:
            raise SettingsError(f"{path}: not an .import.toml, a .scene.toml, an .anim.toml or a folder")
    return sorted(found)


def add_entry(entries, key, source, root):
    """Adds entry `key` from `source`; ids are unique within a pack, whatever their type."""
    if entries.setdefault(key, source) != source:
        raise SettingsError(f"{root}: {entries[key]} and {source} both make entry {key!r}: "
                            "ids are unique within a pack, so rename one")


def scene_entries(path, scene):
    """{entry id: its source} of a scene's pack: its entry, its meshes and every camera's clip."""
    entries = {}
    add_entry(entries, scene_asset.scene_id(path), path, path)
    for item in scene.renderers:
        add_entry(entries, item.asset_name, item.asset_path, path)
    for camera in (obj for obj in scene.objects if obj.kind == "camera"):
        clip = camera.component.path
        if clip:
            add_entry(entries, clip.clip, clip.animation, path)
    return entries


def pack_files(paths):
    """{pack name: {entry id: its source}}, one pack per root under `paths`;
    a mesh's source is its .mesh file, a scene's its .scene.toml, a clip's
    its .anim.toml."""
    return pack_jobs(paths)[0]


def pack_jobs(paths):
    """pack_files() and, for each mesh source, the job that bakes it:
    {resolved .mesh path: (job, its scene or None, the file that asks for it)}."""
    files = input_files(paths)
    roots, placed, jobs = {}, set(), {}
    for path in files:
        if path.name.endswith(SCENE):
            scene = load_scene(path)
            placed.update(item.settings.path for item in scene.renderers)
            roots[path] = scene_entries(path, scene)
            jobs.update((item.asset_path.resolve(), (item, scene, path)) for item in scene.renderers)
            placed.update(source.resolve() for source in roots[path].values() if source.name.endswith(CLIP))
    for path in files:
        if path.name.endswith(IMPORT) and path.resolve() not in placed:
            settings = load_import_settings(path)
            roots[path] = {v.name: settings.mesh_dir / f"{v.name}.mesh" for v in settings.variants}
            jobs.update((item.asset_path.resolve(), (item, None, path)) for item in albedo_jobs(settings))
    for path in files:
        if path.name.endswith(CLIP) and path.resolve() not in placed:
            roots[path] = {tracks_asset.clip_id(path): path}
    packs, owner = {}, {}
    for path, entries in roots.items():
        name = path.name.removesuffix(SCENE).removesuffix(IMPORT).removesuffix(CLIP)
        if name in packs:
            raise SettingsError(f"two roots make a pack named {name!r}: {owner[name]} and {path}")
        owner[name] = path
        packs[name] = entries
    holder = {}
    for name, entries in packs.items():
        for entry in entries:
            if entry in holder:
                raise SettingsError(f"{entry!r} is named by packs {holder[entry]!r} and {name!r}: "
                                    "a shared asset needs a pack of its own, which is not built yet")
            holder[entry] = name
    return packs, jobs


def pack_bytes(paths, replace=(), cache=None, offline=False, max_influences=skin_asset.DEFAULT_INFLUENCES):
    """{pack name: its bytes}. Every mesh comes from the bake cache by its locked key (bake/bake.py),
    `cache` or the user cache, downloading what it lacks unless `offline`; each --replace NAME=FILE takes
    mesh NAME from FILE instead and is never fetched. The lock is this repository's: a mesh of a scene
    or import outside it (a test's or a scratch scene) is the file beside it."""
    packs, jobs = pack_jobs(paths)
    replaced = {}
    for item in replace:
        mesh, _, file = item.partition("=")
        holder = next((entries for entries in packs.values() if mesh in entries), None)
        if holder is None or holder[mesh].name.endswith((SCENE, CLIP)):
            raise SettingsError(f"--replace {mesh}: no such mesh")
        replaced[mesh] = pathlib.Path(file)
    from bake import bake

    locked = {path: job for path, job in jobs.items() if path.is_relative_to(REPO)}
    wanted = [found for found in bake.bakes_in(packs, locked)
              if found.kind != "blend" and found.output.removesuffix(bake.MESH_SUFFIX) not in replaced]
    fetched = {found.tree.resolve(): path for found, path in
               bake.fetch_all(wanted, bake.read_lock(), cache or bake.default_cache(), offline).items()}
    unkeyed = [f"{name}/{entry}" for name, entries in packs.items() for entry, source in entries.items()
               if not source.name.endswith((SCENE, CLIP)) and entry not in replaced
               and source.resolve() in locked and source.resolve() not in fetched]
    if unkeyed:
        raise bake.BakeMissing("no bake keys these meshes, so the cache cannot give them and the tree's "
                               "copy is never taken: " + ", ".join(unkeyed))
    original_packs = packs
    packs = {name: {entry: fetched.get(source.resolve(), source) for entry, source in entries.items()}
             for name, entries in packs.items()}
    for name, entries in packs.items():
        entries.update((mesh, file) for mesh, file in replaced.items() if mesh in entries)
    missing = [str(source) for sources in packs.values() for source in sources.values() if not source.is_file()]
    if missing:
        raise SettingsError("no baked mesh at " + ", ".join(missing) + "; run mesh_import.py first")
    from r3d import skin_pack

    result = {}
    for name, sources in sorted(packs.items()):
        entry_rows = [pack_entry(key, source) for key, source in sorted(sources.items())]
        mesh_rows = [(key, original_packs[name][key], data) for key, kind, data in entry_rows
                     if kind == LIT_MESH and original_packs[name][key].resolve() in jobs]
        additions = skin_pack.entries(mesh_rows, jobs, cache or bake.default_cache(), offline, max_influences)
        skin_pack.append(entry_rows, additions, dict(original_packs[name]))
        result[name] = build_pack(sorted(entry_rows))
    return result


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
        return True
    return False


def write_packs(out, packs, image=None):
    """Writes each pack to out/<name>.apak, removing any other .apak there, and
    with `image` the partition image; returns the names of the packs whose
    bytes changed. Every pack is parsed first, so a bad one is never written."""
    for pack in packs.values():
        parse_pack(pack)
    out.mkdir(parents=True, exist_ok=True)
    for stale in out.glob("*" + PACK_SUFFIX):
        if stale.name.removesuffix(PACK_SUFFIX) not in packs:
            stale.unlink()
    changed = [name for name, pack in packs.items() if write_if_changed(out / f"{name}{PACK_SUFFIX}", pack)]
    if image is not None:
        data = build_directory(sorted(packs.items()))
        parse_directory(data)
        image.parent.mkdir(parents=True, exist_ok=True)
        write_if_changed(image, data)
    return changed


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", help="import files, scene files or folders; launcher/main when omitted")
    parser.add_argument("-o", "--out", help="the folder the packs are written to")
    parser.add_argument("--image", help="also write the partition image: the pack directory and every pack")
    parser.add_argument("--replace", action="append", default=[], metavar="NAME=FILE",
                        help="take mesh NAME from FILE: a scratch bake beside the committed ones")
    parser.add_argument("--pack-of", metavar="ID", help="print the pack that holds entry ID and write nothing")
    parser.add_argument("--bake-cache", metavar="DIR", help="the bake cache; the user cache when omitted")
    parser.add_argument("--offline", action="store_true", help="never download a bake the cache lacks")
    parser.add_argument("--max-influences", type=int, choices=skin_asset.INFLUENCES, default=skin_asset.DEFAULT_INFLUENCES,
                        help="heaviest skin influences per vertex (uncached skin step)")
    args = parser.parse_args(argv)
    paths = args.paths or [DEFAULT_SEARCH]
    try:
        if args.pack_of:
            holder = [name for name, entries in pack_files(paths).items() if args.pack_of in entries]
            if not holder:
                parser.error(f"no pack holds entry {args.pack_of!r}")
            print(holder[0])
            return 0
        if not args.out:
            parser.error("-o DIR is required")
        cache = pathlib.Path(args.bake_cache) if args.bake_cache else None
        packs = pack_bytes(paths, args.replace, cache, args.offline, args.max_influences)
        for name in write_packs(pathlib.Path(args.out), packs, pathlib.Path(args.image) if args.image else None):
            print(f"wrote {name}{PACK_SUFFIX} ({len(packs[name])} bytes): " + ", ".join(sorted(parse_pack(packs[name]))))
    except (SettingsError, PackError, tracks_asset.TracksError, scene_asset.SceneError) as error:
        parser.error(str(error))
    except Exception as error:
        if type(error).__name__ != "BakeMissing":
            raise
        print(f"build_pack.py: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
