"""Read a binary glTF 2.0 file's meshes as one static triangle list.

Standard library only, like gltf_skin.py, whose node walk it shares. Every
node that holds a mesh contributes its triangle primitives, placed by the
node's world transform; a skinned node's are taken as authored, which is its
bind pose (glTF ignores a skinned node's own transform). A vertex's colour is
linear: COLOR_0, or white, times its material's base colour factor.
"""

import pathlib
import sys
from types import SimpleNamespace

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf.gltf_read import load_asset, read_accessor  # noqa: E402
from r3d.gltf_skin import node_parents, node_trs, transform_point, world_matrices  # noqa: E402

TRIANGLES = 4  # a primitive's mode for a triangle list
WHITE = (1.0, 1.0, 1.0)

# The material a primitive without one is named for.
DEFAULT_MATERIAL = "default"


def material_names(document):
    """One name per material; a nameless one is named for its index."""
    return [material.get("name", f"material_{index}") for index, material in enumerate(document.get("materials", []))]


def base_colour(document, material):
    if material is None:
        return WHITE
    factor = document["materials"][material].get("pbrMetallicRoughness", {}).get("baseColorFactor", (*WHITE, 1.0))
    return tuple(factor[:3])


def load_gltf_mesh(path):
    """positions, linear vertex colours, triangles (counter-clockwise, as
    glTF winds them), each triangle's material index and the material names."""
    document, binary = load_asset(path)
    nodes = document.get("nodes", [])
    world = world_matrices(node_parents(nodes), [node_trs(node) for node in nodes])
    names = material_names(document)
    positions, colors, tri_v, tri_m = [], [], [], []
    for index, node in enumerate(nodes):
        if "mesh" not in node:
            continue
        for primitive in document["meshes"][node["mesh"]]["primitives"]:
            if primitive.get("mode", TRIANGLES) != TRIANGLES:
                raise ValueError(f"{path}: node {node.get('name', index)!r} has a primitive that is not triangles")
            attributes = primitive["attributes"]
            points = read_accessor(document, binary, attributes["POSITION"])
            first = len(positions)
            positions += points if "skin" in node else [transform_point(world[index], p) for p in points]
            tint = base_colour(document, primitive.get("material"))
            painted = read_accessor(document, binary, attributes["COLOR_0"]) if "COLOR_0" in attributes else None
            colors += [tuple(c * t for c, t in zip(painted[i][:3] if painted else WHITE, tint)) for i in range(len(points))]
            flat = ([i[0] for i in read_accessor(document, binary, primitive["indices"])] if "indices" in primitive
                    else list(range(len(points))))
            if "material" not in primitive and DEFAULT_MATERIAL not in names:
                names.append(DEFAULT_MATERIAL)
            material = primitive["material"] if "material" in primitive else names.index(DEFAULT_MATERIAL)
            tri_v += [(first + flat[i], first + flat[i + 1], first + flat[i + 2]) for i in range(0, len(flat), 3)]
            tri_m += [material] * (len(flat) // 3)
    if not tri_v:
        raise ValueError(f"{path}: no mesh holds a triangle")
    return SimpleNamespace(positions=positions, colors=colors, tri_v=tri_v, tri_m=tri_m, names=names)
