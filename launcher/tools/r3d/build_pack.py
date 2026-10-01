#!/usr/bin/env python3
"""Write the asset pack from every baked mesh the import and scene files name.

    python launcher/tools/r3d/build_pack.py -o assets.bin [PATH ...]

Each PATH is an .import.toml, a .scene.toml or a folder searched for both;
with none, launcher/main is searched. A mesh named by a file is the entry
<name>.mesh that mesh_import.py wrote beside it, and its pack id is that
name. Run from the repository root; standard library only, and no model is
baked. The pack is a build product, never committed: the firmware build,
the host tests and the render scripts each write their own.
"""

import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from asset.asset_pack import PackError, build_pack, parse_pack  # noqa: E402
from r3d.import_settings import SettingsError, load_import_settings, load_scene  # noqa: E402
from r3d.mesh_asset import TYPE as LIT_MESH  # noqa: E402

REPO = pathlib.Path(__file__).resolve().parents[3]
DEFAULT_SEARCH = REPO / "launcher" / "main"
SUFFIXES = (".import.toml", ".scene.toml")


def input_files(paths):
    """The import and scene files under `paths`, each folder searched."""
    found = []
    for path in map(pathlib.Path, paths):
        if path.is_dir():
            found += sorted(p for p in path.rglob("*.toml") if p.name.endswith(SUFFIXES))
        elif path.name.endswith(SUFFIXES):
            found.append(path)
        else:
            raise SettingsError(f"{path}: not an .import.toml, a .scene.toml or a folder")
    return found


def mesh_files(paths):
    """{mesh name: its .mesh file} for every mesh the files name."""
    meshes = {}
    for path in input_files(paths):
        if path.name.endswith(".scene.toml"):
            sources = [(item.settings, item.variant) for item in load_scene(path).renderers]
        else:
            settings = load_import_settings(path)
            sources = [(settings, variant) for variant in settings.variants]
        for settings, variant in sources:
            entry = settings.mesh_dir / f"{variant.name}.mesh"
            if meshes.setdefault(variant.name, entry) != entry:
                raise SettingsError(f"two import files write a mesh named {variant.name!r}")
    return meshes


def pack_bytes(paths):
    meshes = mesh_files(paths)
    missing = [str(entry) for entry in meshes.values() if not entry.is_file()]
    if missing:
        raise SettingsError("no baked mesh at " + ", ".join(missing) + "; run mesh_import.py first")
    return build_pack([(name, LIT_MESH, entry.read_bytes()) for name, entry in sorted(meshes.items())])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("paths", nargs="*", help="import files, scene files or folders; launcher/main when omitted")
    parser.add_argument("-o", "--out", required=True, help="the pack to write")
    args = parser.parse_args(argv)
    out = pathlib.Path(args.out)
    try:
        pack = pack_bytes(args.paths or [DEFAULT_SEARCH])
        entries = parse_pack(pack)
    except (SettingsError, PackError) as error:
        parser.error(str(error))
    out.parent.mkdir(parents=True, exist_ok=True)
    if not out.is_file() or out.read_bytes() != pack:
        out.write_bytes(pack)
    print(f"wrote {out} ({len(pack)} bytes): " + ", ".join(sorted(entries)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
