# r3d

The offline half of `main/render/`'s r3d renderer: the Python modules that bake
a mesh into asset-pack entries, and host tools that pose, preview or measure a
mesh. Nothing here runs on the board.

| Module | What it does |
|---|---|
| [obj.py](obj.py) | Loads a Wavefront OBJ and its MTL, and the textures the MTL names as mip chains. |
| [geometry.py](geometry.py) | Welding, compaction, corner normals, closest point on a triangle. |
| [tessellate.py](tessellate.py) | Conforming edge splits, used by the `seal_seams` join. |
| [repair.py](repair.py) | The join step of the `seal_seams` import option: border vertices within a tolerance are welded and border edges are split at another piece's vertices, so a shared edge is one edge and the simplifier cannot open a crack along it. Positions only; vertices are never merged. |
| [simplify.py](simplify.py) | Appearance-preserving simplification: split evenly, weld across materials, one colour-aware pass with reserved budget shares for small props. `seal_seams=True` joins touching pieces first, regularizes lightly and merges near colours. |
| [meshopt.py](meshopt.py) | [meshoptimizer](https://github.com/zeux/meshoptimizer)'s simplifier and meshlet clusterizer through ctypes, built once from the pinned `third_party/upstream/meshoptimizer` submodule into `.cache/`. |
| [light.py](light.py) | Baked direct light from a scene's typed lights (`LIGHTS`): directional with soft shadows, sky visibility and ambient, albedo from textures, and culling of what no point in a region can see. |
| [octree.py](octree.py) | Groups weighted items, here meshlets, into an octree whose leaves hold runs of them. |
| [build_pack.py](build_pack.py) | Writes the [asset pack](../../../docs/assets/README.md) (`-o PACK`) from the `.mesh` entries every import and scene file names, with the container writer in [`tools/asset/`](../asset/asset_pack.py). Standard library only. |
| [mesh_asset.py](mesh_asset.py) | The lit mesh entry's type and byte layout, shared by the baker and the pack builder. Standard library only. |
| [lit_mesh.py](lit_mesh.py) | `write_lit_mesh()`: cuts a lit mesh into meshlets under an octree, quantizes it, checks it against `r3d_lit_mesh.h`'s invariants and writes it as a `<name>.mesh` pack entry; a flat import carries one RGB565 colour per face and welds positions without colour seams. The size defaults live here and nowhere else. `read_lit_mesh()` reads an entry back. |
| [import_settings.py](import_settings.py) | Reads and validates an import file and a scene file; standard library only, every table closed. |
| [mesh_import.py](mesh_import.py) | Bakes an import file, or the meshes a scene file places: fetches and checks the source, runs the steps the import opts into, lights with the scene's lights, then writes the `.mesh` entry beside the import file. |
| [rebake.py](rebake.py) | Rewrites a baked `.mesh`'s clusters from its own triangles and colours, with no relighting. |
| [fetch.py](fetch.py) | Downloads a source model once into `.cache/`, checked against a SHA-256. |
| [gltf_skin.py](gltf_skin.py) | Reads a binary glTF 2.0 and poses its skinned mesh on the CPU: accessors, node tree, one skin, animation sampling (LINEAR, STEP, CUBICSPLINE), linear-blend skinning; reads through [`tools/gltf/`](../gltf/gltf_read.py), the reader and reference sampler [`tools/anim/`](../anim/README.md) shares. Standard library only. |
| [gltf_preview.py](gltf_preview.py) | Renders any skinned `.glb` with Pillow: a looping GIF of one animation (`--gif NAME`) or the bind pose from four sides (`--sheet`). |
| [triangle_sizes.c](triangle_sizes.c) | A baked mesh's drawn triangles by the pixel centres they cover from a view, and the poses file; host-tested by `suite_r3d_triangle_sizes.c`. |
| [triangle_sizes_main.c](triangle_sizes_main.c), [report_triangle_sizes.sh](report_triangle_sizes.sh) | The tool over a mesh and a poses file; see [Triangle sizes](#triangle-sizes). |
| [bake_fidelity.py](bake_fidelity.py) | Re-lights a flat mesh's geometry with chosen sample count, placement, sun and sky rays into a scratch directory, renders it on the host and scores it against the reference; see [Sweeping the flat bake](../../../docs/render/Mesh-Import.md#sweeping-the-flat-bake). |
| [reference_render.py](reference_render.py) | Traces the undecimated source mesh through the scene's bake lights at supersampled device resolution; writes linear arrays and RGB565-expanded PNGs for fidelity comparisons. |

The environment is pinned in [requirements.txt](requirements.txt), and the
simplifier needs the meshoptimizer submodule and a host C++ compiler (`CXX`,
else `c++` or `g++`). From `launcher/`:

```sh
git submodule update --init ../third_party/upstream/meshoptimizer
python -m venv tools/r3d/.cache/venv
tools/r3d/.cache/venv/Scripts/python -m pip install -r tools/r3d/requirements.txt   # bin/python on Linux
```

**`rebake.py` or a full import.** Rebake a `.mesh` when only clustering
or data format changes: it reads its triangles and colours back and rewrites
the clusters in place. Run `build_pack.py` after either. Re-import a scene when a mesh's source,
simplification or light changes. Both commands are fixed points: the former
canonicalizes triangle order and the latter uses the import file's fixed seed.

**The `seal_seams` import option.** `simplify(seal_seams=True)`, off by default,
is another way to import the same mesh, with fewer empty pixel-sized spots at
the price of frame time; what it does and costs is in
[Mesh-Import.md](../../../docs/render/Mesh-Import.md#sealing-seams). An import turns it on with `seal_seams = true` in `[process.simplify]`.

`mesh_import.py` is the shared full-import command. Each import file, the
scene file that places it and the `.mesh` it bakes live in the app's `meshes/`
folder.

## Fidelity reference

`reference_render.py` takes a scene and the pose file its camera path emits.
It uses the source mesh after alpha masking, before simplification, and the
same lights, albedo sampling, tone map and RGB565 quantisation as the bake.
The .npy output retains the linear, box-filtered image; the PNG is the form a
host render compares with `render_compare.py`. What the scores mean is in
[Mesh-Import.md](../../../docs/render/Mesh-Import.md#fidelity-against-a-reference).

These commands run from `launcher/`; `PY` is the venv's interpreter
(`tools/r3d/.cache/venv/Scripts/python` on Windows, `.../bin/python` elsewhere).
`TRACKS` is the scene camera's baked track source, `HOST` the scene's
host-render script and `SCENE` its scene file.

```sh
tools/anim/sample_tracks.sh --tracks TRACKS.c:NAME --every 5000 --until 40000     --poses camera 184 224 0.62 6 > poses.txt
$PY tools/r3d/reference_render.py SCENE.scene.toml --poses poses.txt --skip 1     --out reference --samples 4
```

`--every 5000 --until 40000` writes nine poses, times 0 to 40000. A host render
of `--frames 8 --dt 5000` records its first frame after one step, so
`--skip 1` leaves the eight poses it shows.

Score a host render's video against the references, with a heatmap per frame
and a sheet of two frames:

```sh
$PY tools/render/render_compare.py --out unused.png --reference-video host.avi reference     --reference-scale 2 --heatmap-dir heatmaps --reference-sheet sheet.png --sheet-frames 2,4
```

The line for each frame, and for the frames' mean, has mean and 95th-percentile
CIE76 ΔE over its pixels, luma SSIM, and the mean ΔE on edge and on interior
pixels; the frames-mean line averages each frame's value, p95 included. The
sheet is, left to right, the reference, the render, the ΔE heatmap and the
reference's edge pixels in magenta, over the heatmap's colour scale: ΔE 0 is
black, about 20 red, 50 or more yellow.

`bake_fidelity.py` does the whole loop for flat variants of one mesh, and
sweeps the flat bake's knobs:

```sh
$PY tools/r3d/bake_fidelity.py SCENE.scene.toml --mesh FLAT_MESH --script HOST     --render-args "--quarter 0 --no-hud --scene FLAT_SCENE --frames 8 --dt 5000"     --reference reference --work scratch     --variant fixed4=samples=fixed:4 --variant sky64=sky=64,place=centroid
```

It bakes the simplified geometry once, re-lights it for each variant into
`scratch/` (nothing tracked is written), packs that mesh in place of the
tracked one for the host renderer (`AUTANA_ASSET_PACK`), renders
the poses and scores them with `render_compare.py`. A variant is
`samples=fixed:N` or `auto:MIN:MAX:AREA` (`median*K` for AREA), `sky=N`,
`place=stratified|centroid` and `sun=disc|centre`. Without `--variant` the
mesh is baked as its import file declares it, byte for byte the tracked bake.
It prints a table sorted by mean ΔE.

## Triangle sizes

```sh
./launcher/tools/r3d/report_triangle_sizes.sh --mesh NAME POSES|- [--write DIR | --against DIR]
```

How many of a baked mesh's drawn triangles cover 0, 1, 2-4 or more pixel
centres at each pose, which sizes the rasterizer's small-triangle work.
`--mesh` names the mesh's asset id in a pack built from the tree, or in the pack `AUTANA_ASSET_PACK` names;
`POSES` is a text file of `size`, `lens` and `pose` lines, its format in
[`triangle_sizes.h`](triangle_sizes.h), and `-` reads it from standard
input: a scene prints its poses from its own camera rather than keeping a
copy that can go stale.
`--write` keeps each pose's frame and `--against` diffs a later build's
frames with them, pixel by pixel.
