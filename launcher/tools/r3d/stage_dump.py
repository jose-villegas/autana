#!/usr/bin/env python3
"""SCRATCH (5eu5): bake one mesh and print a hash of every stage's output and every ray query."""
import hashlib
import json
import os
import pathlib
import sys
import time

import numpy as np

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from r3d import mesh_import, ray_query  # noqa: E402
from r3d.import_settings import load_scene  # noqa: E402

OUT = []
DUMP = pathlib.Path(sys.argv[3]) if len(sys.argv) > 3 else None


def digest(value):
    h = hashlib.sha256()

    def feed(v):
        if isinstance(v, np.ndarray):
            h.update(str((v.dtype.str, v.shape)).encode())
            h.update(np.ascontiguousarray(v).tobytes())
        elif isinstance(v, (list, tuple)):
            for item in v:
                feed(item)
        elif hasattr(v, "__dict__"):
            for key in sorted(vars(v)):
                if key not in ("intersector", "bounce", "textures", "materials"):
                    feed(getattr(v, key))
        else:
            h.update(repr(v).encode())
    feed(value)
    return h.hexdigest()[:16]


def record(name, value, **extra):
    row = {"stage": name, "hash": digest(value), **extra}
    OUT.append(row)
    print(json.dumps(row), flush=True)


def wrap(module, name, label=None):
    original = getattr(module, name)

    def wrapper(*args, **kwargs):
        record((label or name) + ":in", [a for a in args if isinstance(a, (np.ndarray, list, tuple))])
        result = original(*args, **kwargs)
        record(label or name, result)
        return result
    setattr(module, name, wrapper)


calls = {}


def traced(cls, name, mask=None):
    original = getattr(cls, name)

    def wrapper(self, *args):
        result = original(self, *args)
        n = calls[name] = calls.get(name, 0) + 1
        shown = result
        if mask:
            hit = result[0]
            shown = (hit, np.where(hit, result[1], 0), np.where(hit, result[2], -1))
        record(f"{name}#{n}", shown, rays=len(args[0]), input=digest([np.asarray(a) for a in args[:2]]))
        return result
    setattr(cls, name, wrapper)


traced(ray_query.RayQuery, "first_hits", mask=True)
traced(ray_query.RayQuery, "blocked")
from r3d import path_bake  # noqa: E402
traced(path_bake.PathLight, "bounce")
for fn in ("load_source", "drop_masked", "visible_triangles", "weld_keeping", "densify", "shade_lit", "simplify"):
    wrap(mesh_import, fn)

scene = load_scene(pathlib.Path(sys.argv[1]).resolve())
job = next(job for job in scene.renderers if job.object.name == sys.argv[2])
start = time.monotonic()
geometry = mesh_import.bake_geometry(job, scene)
record("geometry", [geometry.positions, geometry.rgb, geometry.tris, geometry.tri_mat],
       triangles=len(geometry.tris), seconds=round(time.monotonic() - start, 1))
