"""Convert an FBX file to a plain binary glTF.

    python -m fbx.fbx_to_glb in.fbx out.glb      (from launcher/tools)

glTF is the one intermediate the mesh and animation tools read, so an FBX
source is converted once and read as a .glb (gltf_read.load_asset does that).
The result is metres, +Y up, triangulated; Euler angles and pivots are baked
to translation/rotation/scale keys; a skin keeps its four strongest weights
per vertex, normalised. Materials keep their base colour; textures, custom
properties and everything FBX-only are dropped.
"""

import argparse
import pathlib
import sys

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from fbx import log  # noqa: E402
from fbx.ufbx_scene import FbxScene, NO_MATERIAL, ROTATION, SCALE, TRANSLATION  # noqa: E402
from gltf.gltf_write import build_glb  # noqa: E402

PATH_NAMES = {TRANSLATION: "translation", ROTATION: "rotation", SCALE: "scale"}


def _primitive(mesh, triangles, material):
    """One glTF primitive from the chosen triangles: corners with the same
    attributes are one vertex."""
    corners = (3 * triangles[:, None] + np.arange(3)).reshape(-1)
    columns = [mesh.position[corners], mesh.normal[corners]]
    for optional in (mesh.colour, mesh.joint, mesh.weight):
        if optional is not None:
            columns.append(optional[corners].astype(np.float64))
    table = np.hstack([c.astype(np.float64) for c in columns])
    _, first, inverse = np.unique(table, axis=0, return_index=True, return_inverse=True)
    kept = corners[first]
    primitive = {"positions": mesh.position[kept].tolist(), "normals": mesh.normal[kept].tolist(),
                 "indices": inverse.reshape(-1).tolist()}
    if mesh.colour is not None:
        primitive["colors"] = mesh.colour[kept].tolist()
    if mesh.joint is not None:
        primitive["joints"] = mesh.joint[kept].tolist()
        primitive["weights"] = mesh.weight[kept].tolist()
    if material != NO_MATERIAL:
        primitive["material"] = int(material)
    return primitive


def convert(fbx_path):
    """The .glb bytes of an FBX file."""
    with FbxScene(fbx_path) as scene:
        nodes = []
        for index, node in enumerate(scene.nodes()):
            nodes.append({"name": node.name, "translation": node.translation.tolist(),
                          "rotation": node.rotation.tolist(), "scale": node.scale.tolist()})
            if node.parent >= 0:
                nodes[node.parent].setdefault("children", []).append(index)
        meshes, skins = [], []
        for index, node in enumerate(nodes):
            mesh = scene.mesh(index)
            if mesh is None:
                continue
            triangle_materials = mesh.material
            primitives = [_primitive(mesh, np.flatnonzero(triangle_materials == m), m)
                          for m in np.unique(triangle_materials)]
            node["mesh"] = len(meshes)
            meshes.append({"name": node["name"], "primitives": primitives})
            if mesh.skin:
                node["skin"] = len(skins)
                skins.append({"joints": [joint for joint, _ in mesh.skin],
                              "inverse_binds": [inverse.tolist() for _, inverse in mesh.skin]})
        materials = [{"name": m.name, "color": m.colour.tolist()} for m in scene.materials()]
        animations = []
        for number, animation in enumerate(scene.animations()):
            if not animation.channels:
                continue
            animations.append({"name": animation.name or f"animation_{number}", "channels": [
                {"node": node, "path": PATH_NAMES[path], "times": seconds.tolist(), "values": values.tolist()}
                for node, path, seconds, values in animation.channels]})
    return build_glb(nodes, animations, meshes=meshes, skins=skins, materials=materials)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("fbx", type=pathlib.Path)
    parser.add_argument("glb", type=pathlib.Path)
    args = parser.parse_args()
    data = convert(args.fbx)
    args.glb.write_bytes(data)
    log(f"wrote {args.glb} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
