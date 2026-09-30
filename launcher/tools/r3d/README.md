# r3d

The offline half of `main/render/`'s r3d renderer: mesh baking. Nothing here runs
on the board: a generator imports these modules, bakes a model, and writes
checked-in C data.

| Module | What it does |
|---|---|
| [obj.py](obj.py) | Loads a Wavefront OBJ and its MTL, and the textures the MTL names as mip chains. |
| [geometry.py](geometry.py) | Welding, compaction, vertex and corner normals, closest point on a triangle. |
| [decimate.py](decimate.py) | Quadric decimation to a triangle budget, falling back to vertex clustering where many small disconnected pieces stall it (the `--simplifier quadric` path). |
| [tessellate.py](tessellate.py) | Conforming edge splits, and splitting where baked light changes along an edge (the `--simplifier quadric` path). |
| [simplify.py](simplify.py) | Appearance-preserving simplification: split evenly, weld across materials, one colour-aware pass with reserved budget shares for small props. |
| [meshopt.py](meshopt.py), [meshopt_lod.cpp](meshopt_lod.cpp) | [meshoptimizer](https://github.com/zeux/meshoptimizer)'s simplifier and its cluster LOD example through ctypes, built once from the pinned `third_party/upstream/meshoptimizer` submodule into `.cache/`. |
| [light.py](light.py) | Baked direct light: a sun with soft shadows and sky visibility, albedo from textures, and culling of what no point in a region can see. |
| [octree.py](octree.py) | Groups weighted items, here clusters, into an octree whose leaves hold runs of them. |
| [lit_mesh.py](lit_mesh.py) | `write_lit_mesh()`: cuts a lit mesh into meshlets and coarser levels, quantizes it, checks it against `r3d_lit_mesh.h`'s invariants and writes it as C data; `read_lit_mesh()` reads that data back. |
| [rebake.py](rebake.py) | Rewrites a baked mesh in the current format from its own triangles and colours, with no relighting. |
| [lod_eval.py](lod_eval.py) | What the levels would save at each camera pose of a file: triangles and clusters drawn against the finest level, and the pixel difference of a host render. |
| [fetch.py](fetch.py) | Downloads a source model once into `.cache/`, checked against a SHA-256. |

The environment is pinned in [requirements.txt](requirements.txt), and the
simplifier needs the meshoptimizer submodule and a host C++ compiler (`CXX`,
else `c++` or `g++`). From `launcher/`:

```sh
git submodule update --init ../third_party/upstream/meshoptimizer
python -m venv tools/r3d/.cache/venv
tools/r3d/.cache/venv/Scripts/python -m pip install -r tools/r3d/requirements.txt   # bin/python on Linux
```

`rebake.py` or a full bake: when only what happens after lighting changes,
the format (`--clustering keep`, the default), the clustering (`octree` or
`meshlet`, with `--meshlet-triangles`) or the levels (`--lod`, meshlets only),
`python -m r3d.rebake` rewrites a committed mesh from its own triangles and
colours in seconds. Anything before that stage, the model, its simplification
or its light, needs the generator. The committed meshes have been rebaked but
not regenerated since the clustering options were added, so a full generator
run is still owed.

A generator is a script beside the model's consumer: it loads and bakes the
model with these modules and ends in one `write_lit_mesh()` call. The banner
of each file it writes records the exact command that produced it.
