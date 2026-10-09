"""Write a binary glTF 2.0 holding nodes, cameras, animations and meshes.

Standard library only. It writes what tools/anim/tracks_asset.py and the mesh
readers (r3d/gltf_mesh.py, r3d/gltf_skin.py) read: node TRS, an optional
camera, triangle meshes with a skin, and animation channels on translation,
rotation, scale or, through KHR_animation_pointer, any property path. A
converter from another format and a test that needs a scene of its own both
use it.
"""

import json
import struct

GLB_MAGIC = 0x46546C67
CHUNK_JSON = 0x4E4F534A
CHUNK_BIN = 0x004E4942
FLOAT = 5126
UNSIGNED_SHORT = 5123
UNSIGNED_INT = 5125
ARRAY_BUFFER = 34962
ELEMENT_ARRAY_BUFFER = 34963
WIDTH_NAMES = {1: "SCALAR", 2: "VEC2", 3: "VEC3", 4: "VEC4", 16: "MAT4"}
COMPONENT_FORMATS = {FLOAT: "f", UNSIGNED_SHORT: "H", UNSIGNED_INT: "I"}


def _pad(data, fill):
    return data + fill * (-len(data) % 4)


def build_glb(nodes, animations, cameras=(), meshes=(), skins=(), materials=()):
    """`nodes`: dicts with optional name, translation, rotation, scale, camera,
    mesh, skin and children (node indices; a node nobody lists is a scene root).
    `cameras`: glTF camera dicts. `animations`: dicts with a name and channels,
    each channel a dict with `node` (None for a pointer channel), `path`,
    `pointer`, `interpolation`, `times` and `values`: one tuple per key, or
    three per key (in-tangent, value, out-tangent) for CUBICSPLINE. Returns
    the file's bytes.

    `meshes`: dicts with a name and primitives, each a dict with `positions`
    and `indices` (one tuple per vertex, one int per index), optional
    `normals`, `colors` (rgba), `joints` (four ints), `weights` (four floats)
    per vertex, and an optional `material` index. `skins`: dicts with `joints`
    (node indices) and `inverse_binds` (16 floats each, column-major).
    `materials`: dicts with a name and `color`, the base colour factor."""
    blob = bytearray()
    views, accessors = [], []

    def accessor(rows, width, extra=None, component=FLOAT, target=None):
        pattern = "<%d%s" % (width, COMPONENT_FORMATS[component])
        data = b"".join(struct.pack(pattern, *row) for row in rows)
        view = {"buffer": 0, "byteOffset": len(blob), "byteLength": len(data)}
        if target:
            view["target"] = target
        views.append(view)
        blob.extend(_pad(data, b"\0"))
        entry = {"bufferView": len(views) - 1, "componentType": component,
                 "type": WIDTH_NAMES[width], "count": len(rows)}
        entry.update(extra or {})
        accessors.append(entry)
        return len(accessors) - 1

    out_meshes = []
    for mesh in meshes:
        primitives = []
        for primitive in mesh["primitives"]:
            positions = primitive["positions"]
            low = [min(p[axis] for p in positions) for axis in range(3)]
            high = [max(p[axis] for p in positions) for axis in range(3)]
            attributes = {"POSITION": accessor(positions, 3, {"min": low, "max": high}, target=ARRAY_BUFFER)}
            for key, name, width, component in (("normals", "NORMAL", 3, FLOAT), ("colors", "COLOR_0", 4, FLOAT),
                                                ("joints", "JOINTS_0", 4, UNSIGNED_SHORT),
                                                ("weights", "WEIGHTS_0", 4, FLOAT)):
                if primitive.get(key) is not None:
                    attributes[name] = accessor(primitive[key], width, component=component, target=ARRAY_BUFFER)
            entry = {"attributes": attributes,
                     "indices": accessor([(i,) for i in primitive["indices"]], 1, component=UNSIGNED_INT,
                                         target=ELEMENT_ARRAY_BUFFER)}
            if primitive.get("material") is not None:
                entry["material"] = primitive["material"]
            primitives.append(entry)
        out_meshes.append({"name": mesh.get("name", ""), "primitives": primitives})

    out_skins = [{"joints": list(skin["joints"]), "inverseBindMatrices": accessor(skin["inverse_binds"], 16)}
                 for skin in skins]
    out_materials = [{"name": material.get("name", ""),
                      "pbrMetallicRoughness": {"baseColorFactor": list(material["color"]), "metallicFactor": 0.0}}
                     for material in materials]

    out_animations = []
    pointers = False
    for animation in animations:
        samplers, channels = [], []
        for channel in animation["channels"]:
            times = channel["times"]
            width = len(channel["values"][0])
            samplers.append({
                "input": accessor([(t,) for t in times], 1,
                                  {"min": [min(times)], "max": [max(times)]}),
                "output": accessor(channel["values"], width),
                "interpolation": channel.get("interpolation", "LINEAR"),
            })
            target = {"path": channel["path"]}
            if channel.get("node") is not None:
                target["node"] = channel["node"]
            if channel.get("pointer"):
                pointers = True
                target["extensions"] = {"KHR_animation_pointer": {"pointer": channel["pointer"]}}
            channels.append({"sampler": len(samplers) - 1, "target": target})
        out_animations.append({"name": animation.get("name", ""),
                               "samplers": samplers, "channels": channels})

    children = {child for node in nodes for child in node.get("children", ())}
    document = {
        "asset": {"version": "2.0", "generator": "autana tools/anim/gltf_write.py"},
        "scene": 0,
        "scenes": [{"nodes": [i for i in range(len(nodes)) if i not in children]}],
        "nodes": list(nodes),
        "animations": out_animations,
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": views,
        "accessors": accessors,
    }
    for key, value in (("cameras", list(cameras)), ("meshes", out_meshes), ("skins", out_skins),
                       ("materials", out_materials)):
        if value:
            document[key] = value
    if pointers:
        document["extensionsUsed"] = ["KHR_animation_pointer"]
    text = _pad(json.dumps(document, separators=(",", ":")).encode("utf-8"), b" ")
    binary = _pad(bytes(blob), b"\0")
    total = 12 + 8 + len(text) + 8 + len(binary)
    return (struct.pack("<III", GLB_MAGIC, 2, total)
            + struct.pack("<II", len(text), CHUNK_JSON) + text
            + struct.pack("<II", len(binary), CHUNK_BIN) + binary)
