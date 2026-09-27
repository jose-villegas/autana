# Mesh

Offline mesh baking for the firmware's software renderers. Nothing here runs
on the board: a generator imports these modules, bakes a model, and writes
checked-in C data.

| Module | What it does |
|---|---|
| [obj.py](obj.py) | Loads a Wavefront OBJ and its MTL, and the textures the MTL names as mip chains. |
| [geometry.py](geometry.py) | Welding, compaction, vertex and corner normals, closest point on a triangle. |
| [decimate.py](decimate.py) | Quadric decimation to a triangle budget, falling back to vertex clustering where many small disconnected pieces stall it. |
| [tessellate.py](tessellate.py) | Conforming edge splits, and splitting where baked light changes along an edge. |
| [light.py](light.py) | Baked direct light: a sun with soft shadows and sky visibility, albedo from textures, and culling of what no point in a region can see. |
| [octree.py](octree.py) | Groups triangles into an octree whose leaves become clusters. |
| [fetch.py](fetch.py) | Downloads a source model once into `.cache/`, checked against a SHA-256. |

The environment is pinned in [requirements.txt](requirements.txt). From
`launcher/`:

```sh
python -m venv tools/mesh/.cache/venv
tools/mesh/.cache/venv/Scripts/python -m pip install -r tools/mesh/requirements.txt   # bin/python on Linux and macOS
tools/mesh/.cache/venv/Scripts/python main/apps/render_lab/tools/gen_sponza.py --out-dir main/apps/render_lab
```

A generator that uses these modules is
[gen_sponza.py](../../main/apps/render_lab/tools/gen_sponza.py); its banner in
each generated file records the exact command that produced it.
