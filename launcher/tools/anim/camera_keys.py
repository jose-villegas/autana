"""A camera path authored as keys (NAME.keys.toml), built into a glTF binary
in memory; gltf_read.load_asset reads one through load_glb_bytes()."""
import math
import pathlib
import sys
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from gltf.gltf_write import build_glb


def rotation(eye, target):
    """glTF xyzw rotation with local -Z forward and world +Y up."""
    forward = [b - a for a, b in zip(eye, target)]
    if sum(v * v for v in forward) == 0 or forward[0] == forward[2] == 0:
        raise ValueError("look_at must differ from eye and not lie on its vertical axis")
    yaw = math.atan2(-forward[0], -forward[2])
    pitch = math.atan2(forward[1], math.hypot(forward[0], forward[2]))
    sy, cy = math.sin(yaw / 2), math.cos(yaw / 2)
    sp, cp = math.sin(pitch / 2), math.cos(pitch / 2)
    return [cy * sp, sy * cp, -sy * sp, cy * cp]


def build(values):
    """GLB bytes from the keys file's tables, with time-scaled Catmull-Rom tangents."""
    if set(values) != {"node", "animation", "keys"}:
        raise ValueError("expected node, animation and keys")
    if any(not isinstance(values[name], str) or not values[name] for name in ("node", "animation")):
        raise ValueError("node and animation must be nonempty names")
    keys = values["keys"]
    if not isinstance(keys, list) or len(keys) < 2:
        raise ValueError("at least two keys are required")
    times, eyes, rotations = [], [], []
    for key in keys:
        if not isinstance(key, dict) or set(key) != {"t", "eye", "look_at"}:
            raise ValueError("each key needs t, eye and look_at")
        numbers = [key["t"]]
        for name in ("eye", "look_at"):
            if not isinstance(key[name], list) or len(key[name]) != 3:
                raise ValueError(f"{name} must have three coordinates")
            numbers.extend(key[name])
        if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in numbers):
            raise ValueError("key coordinates and times must be finite numbers")
        if (not times and key["t"] != 0) or (times and key["t"] <= times[-1]):
            raise ValueError("times must start at zero and increase strictly")
        q = rotation(key["eye"], key["look_at"])
        if rotations and sum(a * b for a, b in zip(q, rotations[-1])) < 0:
            q = [-v for v in q]
        times.append(key["t"])
        eyes.append(key["eye"])
        rotations.append(q)
    loop = eyes[-1] == eyes[0] and keys[-1]["look_at"] == keys[0]["look_at"]
    translations = []
    for i, eye in enumerate(eyes):
        before, after = max(0, i - 1), min(len(keys) - 1, i + 1)
        dt = times[after] - times[before]
        if loop and i in (0, len(keys) - 1):
            before, after = len(keys) - 2, 1
            dt = times[-1] - times[before] + times[after]
        tangent = [(b - a) / dt for a, b in zip(eyes[before], eyes[after])]
        translations.extend((tangent, eye, tangent))
    channels = [dict(node=0, path="translation", interpolation="CUBICSPLINE", times=times, values=translations),
                dict(node=0, path="rotation", interpolation="LINEAR", times=times, values=rotations)]
    return build_glb([dict(name=values["node"], camera=0)],
                     [dict(name=values["animation"], channels=channels)],
                     [dict(type="perspective", perspective=dict(yfov=1.0, znear=0.1))])


def load_glb_bytes(path):
    """GLB bytes of a keys file; every fault is a ValueError."""
    try:
        return build(tomllib.loads(pathlib.Path(path).read_text(encoding="utf-8")))
    except OverflowError as error:
        raise ValueError(str(error)) from error
