"""Read a binary glTF 2.0 file and pose its skinned mesh on the CPU.

Standard library only, so asset tests and bakers can share it without the
pinned environment. It covers what a skinned mesh uses: accessors (any
component type, normalized or not, strided or packed), the node tree's TRS,
the first skinned primitive (normals and indices optional), animation
channels on translation/rotation/scale with LINEAR, STEP or CUBICSPLINE
samplers (also read for any glTF animation by tools/anim/, pointer-targeted
channels included), and linear-blend skinning.
"""

import json
import math
import struct

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942

COMPONENT_FORMATS = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
TYPE_WIDTHS = {"SCALAR": 1, "VEC2": 2, "VEC3": 3, "VEC4": 4, "MAT4": 16}
NORMALIZED_DIVISORS = {5120: 127.0, 5121: 255.0, 5122: 32767.0, 5123: 65535.0}


def load_glb(path):
    """Return (document, binary chunk) of a .glb file."""
    with open(path, "rb") as handle:
        data = handle.read()
    return parse_glb(data)


def parse_glb(data):
    magic, version, length = struct.unpack_from("<III", data, 0)
    if magic != GLB_MAGIC or version != 2 or length != len(data):
        raise ValueError("not a glTF 2.0 binary of the stated length")
    offset = 12
    document = None
    binary = b""
    while offset < length:
        chunk_length, chunk_type = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8:offset + 8 + chunk_length]
        if chunk_type == CHUNK_JSON:
            document = json.loads(chunk.decode("utf-8"))
        elif chunk_type == CHUNK_BIN:
            binary = chunk
        offset += 8 + chunk_length
    if document is None:
        raise ValueError("glb has no JSON chunk")
    return document, binary


def read_accessor(document, binary, index):
    """Return an accessor's elements as a list of tuples of numbers."""
    accessor = document["accessors"][index]
    fmt, size = COMPONENT_FORMATS[accessor["componentType"]]
    width = TYPE_WIDTHS[accessor["type"]]
    view = document["bufferViews"][accessor["bufferView"]]
    base = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    stride = view.get("byteStride", size * width)
    divisor = None
    if accessor.get("normalized"):
        divisor = NORMALIZED_DIVISORS[accessor["componentType"]]
    element = struct.Struct("<" + fmt * width)
    out = []
    for i in range(accessor["count"]):
        values = element.unpack_from(binary, base + i * stride)
        if divisor is not None:
            values = tuple(max(v / divisor, -1.0) for v in values)
        out.append(values)
    return out


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


def quat_slerp(a, b, t):
    dot = sum(x * y for x, y in zip(a, b))
    if dot < 0.0:
        b = tuple(-x for x in b)
        dot = -dot
    if dot > 0.9995:
        out = tuple(x + (y - x) * t for x, y in zip(a, b))
    else:
        theta = math.acos(dot)
        sa = math.sin((1.0 - t) * theta) / math.sin(theta)
        sb = math.sin(t * theta) / math.sin(theta)
        out = tuple(sa * x + sb * y for x, y in zip(a, b))
    norm = math.sqrt(sum(x * x for x in out))
    return tuple(x / norm for x in out)


def sample_keys(times, values, time, interpolation="LINEAR", quaternion=False):
    """One glTF animation sampler evaluated at `time` seconds, clamped at
    both ends. `values` holds one tuple per key, or for CUBICSPLINE three per
    key: in-tangent, value, out-tangent."""
    cubic = interpolation == "CUBICSPLINE"
    value = (lambda k: values[3 * k + 1]) if cubic else (lambda k: values[k])
    if time <= times[0] or len(times) == 1:
        return value(0)
    if time >= times[-1]:
        return value(len(times) - 1)
    hi = next(i for i, t in enumerate(times) if t >= time)
    lo = hi - 1
    if interpolation == "STEP":
        return value(lo)
    dt = times[hi] - times[lo]
    s = (time - times[lo]) / dt
    if cubic:
        s2, s3 = s * s, s * s * s
        out = tuple(
            (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * dt * m0
            + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * dt * m1
            for p0, m0, p1, m1 in zip(
                values[3 * lo + 1], values[3 * lo + 2],
                values[3 * hi + 1], values[3 * hi]
            )
        )
        if quaternion:
            norm = math.sqrt(sum(x * x for x in out))
            out = tuple(x / norm for x in out)
        return out
    if quaternion:
        return quat_slerp(values[lo], values[hi], s)
    return tuple(a + (b - a) * s for a, b in zip(values[lo], values[hi]))


def read_animation(document, binary, animation):
    """The channels of one animation dict: each a dict with `node` (None for a
    pointer channel), `path`, `pointer` (None unless one), `interpolation`,
    `times` and `values`."""
    channels = []
    for channel in animation["channels"]:
        sampler = animation["samplers"][channel["sampler"]]
        target = channel["target"]
        pointer = target.get("extensions", {}).get("KHR_animation_pointer", {}).get("pointer")
        channels.append({
            "node": target.get("node"),
            "path": target["path"],
            "pointer": pointer,
            "interpolation": sampler.get("interpolation", "LINEAR"),
            "times": [t[0] for t in read_accessor(document, binary, sampler["input"])],
            "values": read_accessor(document, binary, sampler["output"]),
        })
    return channels


class SkinnedAsset:
    """The first skinned mesh primitive of a glTF document, poseable."""

    def __init__(self, document, binary):
        self.document = document
        self.binary = binary
        self._accessors = {}
        nodes = document["nodes"]
        self.parents = [None] * len(nodes)
        for index, node in enumerate(nodes):
            for child in node.get("children", []):
                self.parents[child] = index
        self.rest = [self._node_trs(node) for node in nodes]
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

    @staticmethod
    def _node_trs(node):
        return {
            "translation": tuple(node.get("translation", (0.0, 0.0, 0.0))),
            "rotation": tuple(node.get("rotation", (0.0, 0.0, 0.0, 1.0))),
            "scale": tuple(node.get("scale", (1.0, 1.0, 1.0))),
        }

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
        world = [None] * len(pose)

        def resolve(index):
            if world[index] is None:
                trs = pose[index]
                local = mat_from_trs(trs["translation"], trs["rotation"], trs["scale"])
                parent = self.parents[index]
                world[index] = local if parent is None else mat_mul(resolve(parent), local)
            return world[index]

        for index in range(len(pose)):
            resolve(index)
        return world

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
