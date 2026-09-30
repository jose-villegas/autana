# r3d

The offline half of `main/render/`'s r3d renderer: the Python modules that bake
a mesh into checked-in C data, and host tools that measure a baked mesh.
Nothing here runs on the board.

| Module | What it does |
|---|---|
| [obj.py](obj.py) | Loads a Wavefront OBJ and its MTL, and the textures the MTL names as mip chains. |
| [geometry.py](geometry.py) | Welding, compaction, vertex and corner normals, closest point on a triangle. |
| [decimate.py](decimate.py) | Quadric decimation to a triangle budget, falling back to vertex clustering where many small disconnected pieces stall it (the `--simplifier quadric` path). |
| [tessellate.py](tessellate.py) | Conforming edge splits, and splitting where baked light changes along an edge (the `--simplifier quadric` path). |
| [simplify.py](simplify.py) | Appearance-preserving simplification: split evenly, weld across materials, one colour-aware pass with reserved budget shares for small props. |
| [meshopt.py](meshopt.py) | [meshoptimizer](https://github.com/zeux/meshoptimizer)'s simplifier through ctypes, built once from the pinned `third_party/upstream/meshoptimizer` submodule into `.cache/`. |
| [light.py](light.py) | Baked direct light: a sun with soft shadows and sky visibility, albedo from textures, and culling of what no point in a region can see. |
| [octree.py](octree.py) | Groups triangles into an octree whose leaves become clusters. |
| [lit_mesh.py](lit_mesh.py) | `write_lit_mesh()`: clusters a lit mesh, quantizes it, checks it against `r3d_lit_mesh.h`'s invariants and writes it as C data. |
| [fetch.py](fetch.py) | Downloads a source model once into `.cache/`, checked against a SHA-256. |
| [triangle_sizes.c](triangle_sizes.c) | A baked mesh's drawn triangles by the pixel centres they cover from a view, and the poses file; host-tested by `suite_r3d_triangle_sizes.c`. |
| [triangle_sizes_main.c](triangle_sizes_main.c), [report_triangle_sizes.sh](report_triangle_sizes.sh) | The tool over a mesh and a poses file; see [Triangle sizes](#triangle-sizes). |

The environment is pinned in [requirements.txt](requirements.txt), and the
simplifier needs the meshoptimizer submodule and a host C++ compiler (`CXX`,
else `c++` or `g++`). From `launcher/`:

```sh
git submodule update --init ../third_party/upstream/meshoptimizer
python -m venv tools/r3d/.cache/venv
tools/r3d/.cache/venv/Scripts/python -m pip install -r tools/r3d/requirements.txt   # bin/python on Linux
```

A generator is a script beside the model's consumer: it loads and bakes the
model with these modules and ends in one `write_lit_mesh()` call. The banner
of each file it writes records the exact command that produced it.

## Triangle sizes

```sh
./launcher/tools/r3d/report_triangle_sizes.sh --mesh SOURCE.c:SYMBOL POSES|- [--write DIR | --against DIR]
```

How many of a baked mesh's drawn triangles cover 0, 1, 2-4 or more pixel
centres at each pose, which sizes the rasterizer's small-triangle work.
`--mesh` names the C file a generator wrote and its `r3d_lit_mesh_t`;
`POSES` is a text file of `size`, `lens` and `pose` lines, its format in
[`triangle_sizes.h`](triangle_sizes.h), and `-` reads it from standard
input: a scene prints its poses from its own camera rather than keeping a
copy that can go stale.
`--write` keeps each pose's frame and `--against` diffs a later build's
frames with them, pixel by pixel.
