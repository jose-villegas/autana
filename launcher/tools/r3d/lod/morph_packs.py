"""Packs whose levelled meshes carry morph targets, made from the locked bakes: the morph arm of the blend's tables
and figures without a bake of its own.

    python launcher/tools/r3d/lod/morph_packs.py -o DIR MESH [MESH ...]

Each MESH is a pack entry id (build_pack.py --pack-of names its pack). Its locked bundle gets an LMOR beside every
mesh with levels that lacks one (mesh_bundle.mesh_entries, as `[geometry] morph = true` bakes it), and DIR takes the
packs build_pack.py writes with those bundles replaced, for a host renderer's AUTANA_ASSET_DIR.
"""
import argparse
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
TOOLS = HERE.parents[1]
sys.path.insert(0, str(TOOLS))
from r3d import lod_asset, mesh_asset, mesh_bundle, morph_asset  # noqa: E402
from r3d.lod.morph import morph_blob  # noqa: E402

ROOT = TOOLS.parents[1]
BAKE = TOOLS / "bake/bake.py"
BUILD_PACK = TOOLS / "r3d/build_pack.py"


def with_morph(data):
    """Bundle bytes `data` with morph targets for every levelled mesh that has none."""
    entries, copies = mesh_bundle.decode(data)
    names = {name for name, _, _ in entries}
    levelled = [name for name, kind, _ in entries if kind == mesh_asset.TYPE
                and lod_asset.name_for_mesh(name) in names and morph_asset.name_for_mesh(name) not in names]
    rebuilt = {name: mesh_bundle.mesh_entries(name, mesh_bundle.read_mesh(entries, name), morph_blob) for name in levelled}
    replaced = {row[0] for rows in rebuilt.values() for row in rows}
    return mesh_bundle.encode([row for row in entries if row[0] not in replaced]
                              + [row for rows in rebuilt.values() for row in rows], copies)


def locked_bundle(mesh):
    """The cached file of `mesh`'s locked bake, fetched when missing."""
    run = subprocess.run([sys.executable, str(BAKE), "path", mesh + mesh_bundle.SUFFIX], cwd=ROOT,
                         capture_output=True, text=True, check=True)
    return pathlib.Path(run.stdout.strip().splitlines()[-1])


def morph_packs(meshes, out):
    """Write the packs into `out`, `meshes` replaced by their morphing bundles; returns `out`."""
    out = pathlib.Path(out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as scratch:
        replace = []
        for mesh in meshes:
            path = pathlib.Path(scratch) / (mesh + mesh_bundle.SUFFIX)
            path.write_bytes(with_morph(locked_bundle(mesh).read_bytes()))
            replace += ["--replace", f"{mesh}={path}"]
        subprocess.run([sys.executable, str(BUILD_PACK), "-o", str(out), *replace], cwd=ROOT, check=True)
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-o", "--out", required=True, type=pathlib.Path)
    parser.add_argument("meshes", nargs="+", metavar="MESH")
    options = parser.parse_args()
    morph_packs(options.meshes, options.out)


if __name__ == "__main__":
    main()
