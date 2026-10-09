"""Read a binary glTF 2.0 file and pose its skinned mesh on the CPU.

Standard library only, so asset tests and bakers can share it without the
pinned environment. It covers what a skinned mesh uses: accessors (any
component type, normalized or not, strided or packed), the node tree's TRS,
the first skinned primitive (normals and indices optional), animation
channels on translation/rotation/scale with LINEAR, STEP or CUBICSPLINE
samplers (the reader and sampler are tools/gltf/gltf_read.py), and
linear-blend skinning.
"""

import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf.gltf_read import (  # noqa: E402,F401
    load_glb, parse_glb, quat_slerp, read_accessor, sample_keys)

def mat_mul(a, b):
    """Row-major 4x4 product a*b."""
    return [
        sum(a[r * 4 + k] * b[k * 4 + c] for k in range(4))
        for r in range(4)
        for c in range(4)
    ]


def mat_from_column_major(values):
    return [values[c * 4 + r] for r in range(4) for c in range(4)]


def mat_from_trs(translation, rotation, scale):
    x, y, z, w = rotation
    sx, sy, sz = scale
    return [
        (1 - 2 * (y * y + z * z)) * sx, 2 * (x * y - z * w) * sy,
        2 * (x * z + y * w) * sz, translation[0],
        2 * (x * y + z * w) * sx, (1 - 2 * (x * x + z * z)) * sy,
        2 * (y * z - x * w) * sz, translation[1],
        2 * (x * z - y * w) * sx, 2 * (y * z + x * w) * sy,
        (1 - 2 * (x * x + y * y)) * sz, translation[2],
        0.0, 0.0, 0.0, 1.0,
    ]


def transform_point(m, p):
    return (
        m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3],
        m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7],
        m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11],
    )


def transform_vector(m, v):
    return (
        m[0] * v[0] + m[1] * v[1] + m[2] * v[2],
        m[4] * v[0] + m[5] * v[1] + m[6] * v[2],
        m[8] * v[0] + m[9] * v[1] + m[10] * v[2],
    )


def node_parents(nodes):
    """Each node's parent index, None for a root."""
    parents = [None] * len(nodes)
    for index, node in enumerate(nodes):
        for child in node.get("children", []):
            parents[child] = index
    return parents


def node_trs(node):
    return {
        "translation": tuple(node.get("translation", (0.0, 0.0, 0.0))),
        "rotation": tuple(node.get("rotation", (0.0, 0.0, 0.0, 1.0))),
        "scale": tuple(node.get("scale", (1.0, 1.0, 1.0))),
    }


def world_matrices(parents, pose):
    """Each node's model-to-world matrix for a pose of node TRS dicts."""
    world = [None] * len(pose)

    def resolve(index):
        if world[index] is None:
            trs = pose[index]
            local = mat_from_trs(trs["translation"], trs["rotation"], trs["scale"])
            parent = parents[index]
            world[index] = local if parent is None else mat_mul(resolve(parent), local)
        return world[index]

    for index in range(len(pose)):
        resolve(index)
    return world


class SkinnedAsset:
    """The first skinned mesh primitive of a glTF document, poseable."""

    def __init__(self, document, binary):
        self.document = document
        self.binary = binary
        self._accessors = {}
        nodes = document["nodes"]
        self.parents = node_parents(nodes)
        self.rest = [node_trs(node) for node in nodes]
        mesh_node = next(n for n in nodes if "skin" in n and "mesh" in n)
        skin = document["skins"][mesh_node["skin"]]
        self.joints = skin["joints"]
        self.inverse_binds = [
            mat_from_column_major(m)
            for m in read_accessor(document, binary, skin["inverseBindMatrices"])
        ]
        primitive = document["meshes"][mesh_node["mesh"]]["primitives"][0]
        attributes = primitive["attributes"]
        self.positions = self._read(attributes["POSITION"])
        self.normals = self._read(attributes["NORMAL"]) if "NORMAL" in attributes else None
        self.colors = self._read(attributes["COLOR_0"]) if "COLOR_0" in attributes else None
        self.joint_indices = self._read(attributes["JOINTS_0"])
        self.weights = self._read(attributes["WEIGHTS_0"])
        if "indices" in primitive:
            flat = [i[0] for i in self._read(primitive["indices"])]
        else:
            flat = list(range(len(self.positions)))
        self.triangles = [tuple(flat[i:i + 3]) for i in range(0, len(flat), 3)]
        self.animations = {
            a.get("name", str(i)): a for i, a in enumerate(document.get("animations", []))
        }

    def _read(self, accessor_index):
        if accessor_index not in self._accessors:
            self._accessors[accessor_index] = read_accessor(
                self.document, self.binary, accessor_index
            )
        return self._accessors[accessor_index]

    def duration(self, name):
        animation = self.animations[name]
        return max(
            self.document["accessors"][s["input"]]["max"][0]
            for s in animation["samplers"]
        )

    def sample(self, name, time):
        """Node TRS dicts with the named animation applied at `time` seconds."""
        pose = [dict(trs) for trs in self.rest]
        if name is None:
            return pose
        animation = self.animations[name]
        for channel in animation["channels"]:
            sampler = animation["samplers"][channel["sampler"]]
            times = [t[0] for t in self._read(sampler["input"])]
            values = self._read(sampler["output"])
            path = channel["target"]["path"]
            if "node" not in channel["target"] or path not in ("translation", "rotation", "scale"):
                continue
            pose[channel["target"]["node"]][path] = sample_keys(
                times, values, time, sampler.get("interpolation", "LINEAR"), path == "rotation"
            )
        return pose

    def world_matrices(self, pose):
        return world_matrices(self.parents, pose)

    def joint_matrices(self, pose):
        world = self.world_matrices(pose)
        return [
            mat_mul(world[joint], inverse)
            for joint, inverse in zip(self.joints, self.inverse_binds)
        ]

    def skin(self, pose):
        """Skinned (positions, normals) for a pose from sample(); normals is
        None when the mesh has none."""
        matrices = self.joint_matrices(pose)
        positions = []
        normals = [] if self.normals is not None else None
        source_normals = self.normals or [None] * len(self.positions)
        for p, n, js, ws in zip(self.positions, source_normals, self.joint_indices, self.weights):
            blended = [0.0] * 16
            for j, w in zip(js, ws):
                if w > 0.0:
                    m = matrices[j]
                    for k in range(12):
                        blended[k] += m[k] * w
            blended[15] = 1.0
            positions.append(transform_point(blended, p))
            if n is None:
                continue
            nx, ny, nz = transform_vector(blended, n)
            length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
            normals.append((nx / length, ny / length, nz / length))
        return positions, normals
