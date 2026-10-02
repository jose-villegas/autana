# r3d

The offline half of `main/render/`'s r3d renderer: the Python modules that bake
a mesh into checked-in C data, and host tools that pose, preview or measure a
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
| [lit_mesh.py](lit_mesh.py) | `write_lit_mesh()`: cuts a lit mesh into meshlets under an octree, quantizes it, checks it against `r3d_lit_mesh.h`'s invariants and writes it as C data; a flat import carries one RGB565 colour per face and welds positions without colour seams. The size defaults live here and nowhere else. `read_lit_mesh()` reads that data back. |
| [import_settings.py](import_settings.py) | Reads and validates an import file and a scene file; standard library only, every table closed. |
| [mesh_import.py](mesh_import.py) | Bakes an import file, or the meshes a scene file places: fetches and checks the source, runs the steps the import opts into, lights with the scene's lights, then writes the generated C mesh. |
| [rebake.py](rebake.py) | Rewrites a baked mesh's clusters from its own triangles and colours, with no relighting. |
| [fetch.py](fetch.py) | Downloads a source model once into `.cache/`, checked against a SHA-256. |
| [gltf_skin.py](gltf_skin.py) | Reads a binary glTF 2.0 and poses its skinned mesh on the CPU: accessors, node tree, one skin, animation sampling (LINEAR, STEP, CUBICSPLINE), linear-blend skinning; reads through [`tools/gltf/`](../gltf/gltf_read.py), the reader and reference sampler [`tools/anim/`](../anim/README.md) shares. Standard library only. |
| [gltf_preview.py](gltf_preview.py) | Renders any skinned `.glb` with Pillow: a looping GIF of one animation (`--gif NAME`) or the bind pose from four sides (`--sheet`). |
| [triangle_sizes.c](triangle_sizes.c) | A baked mesh's drawn triangles by the pixel centres they cover from a view, and the poses file; host-tested by `suite_r3d_triangle_sizes.c`. |
| [triangle_sizes_main.c](triangle_sizes_main.c), [report_triangle_sizes.sh](report_triangle_sizes.sh) | The tool over a mesh and a poses file; see [Triangle sizes](#triangle-sizes). |
| [bake_fidelity.py](bake_fidelity.py) | Re-lights a flat mesh's geometry with chosen sample count, placement, sun and sky rays into a scratch directory, renders it on the host and scores it against the reference; see [Sweeping the flat bake](../../../docs/render/Mesh-Import.md#sweeping-the-flat-bake). |
| [appearance_simplify.py](appearance_simplify.py) | Fits a smooth mesh's vertex positions and colours to reference renders along a camera path with a differentiable rasterizer, its triangles unchanged; see [Appearance fit](#appearance-fit). |
| [poses.py](poses.py) | Reads the camera poses file `tools/anim/sample_tracks.sh` writes, samples a scene camera's path through it, and casts a pose's pinhole rays. |
| [cost_model.py](cost_model.py) | A linear model of a mesh's frame time from a pose (submitted and drawn triangles, rows, pixels with overdraw, clusters in view), its weights fitted to board captures; see [Cost-aware fit](#cost-aware-fit). |
| [reference_render.py](reference_render.py) | Traces the undecimated source mesh through the scene's bake lights at supersampled device resolution; writes linear arrays and RGB565-expanded PNGs for fidelity comparisons. |

The environment is pinned in [requirements.txt](requirements.txt), and the
simplifier needs the meshoptimizer submodule and a host C++ compiler (`CXX`,
else `c++` or `g++`). From `launcher/`:

```sh
git submodule update --init ../third_party/upstream/meshoptimizer
python -m venv tools/r3d/.cache/venv
tools/r3d/.cache/venv/Scripts/python -m pip install -r tools/r3d/requirements.txt   # bin/python on Linux
```

**`rebake.py` or a full import.** Rebake a generated mesh when only clustering
or data format changes: it reads its triangles and colours back and rewrites
the clusters in place. Re-import a scene when a mesh's source,
simplification or light changes. Both commands are fixed points: the former
canonicalizes triangle order and the latter uses the import file's fixed seed.

**The `seal_seams` import option.** `simplify(seal_seams=True)`, off by default,
is another way to import the same mesh, with fewer empty pixel-sized spots at
the price of frame time; what it does and costs is in
[Mesh-Import.md](../../../docs/render/Mesh-Import.md#sealing-seams). An import turns it on with `seal_seams = true` in `[process.simplify]`.

`mesh_import.py` is the shared full-import command. Each import file and the
scene file that places it live in the app's `meshes/` folder; the generated
banner names the scene file and the exact command that produced it.

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
tools/anim/sample_tracks.sh --tracks TRACKS.c:NAME --every 5000 --until 45000     --poses camera 184 224 0.62 6 > poses.txt
$PY tools/r3d/reference_render.py SCENE.scene.toml --poses poses.txt --skip 1     --out reference --samples 4 --clear RRGGBB
```

`--every 5000 --until 45000` writes nine poses, times 0 to 40000 (`--until`
is exclusive). A host render of `--frames 8 --dt 5000` records its first frame
after one step, so `--skip 1` leaves the eight poses it shows. `--clear` is the
colour the scene clears to, so the sky scores as the host draws it.

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
`scratch/` (nothing tracked is written), builds the host renderer with that
mesh in place of the tracked one (`--substitute` of the scene script), renders
the poses and scores them with `render_compare.py`. A variant is
`samples=fixed:N` or `auto:MIN:MAX:AREA` (`median*K` for AREA), `sky=N`,
`place=stratified|centroid` and `sun=disc|centre`. Without `--variant` the
mesh is baked as its import file declares it, byte for byte the tracked bake.
It prints a table sorted by mean ΔE.

## Appearance fit

`appearance_simplify.py` takes a smooth mesh the simplifier baked at a
triangle budget and moves its vertices and changes their colours until its
renders match the reference along a camera path: appearance-driven
simplification (Hasselgren, Hofmann, Munkberg, Laine, Aila, Lehtinen,
*Appearance-Driven Automatic 3D Model Simplification*, EGSR 2021). The
triangles stay as the simplifier left them, so the budget holds.

| | |
|---|---|
| Start | a smooth `<name>_mesh_generated.c`, welded so the vertices of a colour seam share one position |
| Fitted | every welded position, and every vertex's sRGB colour |
| Forward model | nvdiffrast draws what the device draws: Gouraud colours, single-sided faces culled, the `--clear` colour where nothing is drawn, at `--scale` times the reference size |
| Loss | the mean CIE76 ΔE of `render_compare.py` against the nearest-upscaled reference PNG, over a batch of random poses, plus `--laplacian` times the drift of the positions' uniform-Laplacian coordinates from the start's |
| Schedule | Adam; both learning rates decay tenfold over `--steps` |
| Output | `write_lit_mesh()`, the writer `mesh_import.py` and `rebake.py` end in, plus a vertex-coloured OBJ |

The fitted colours are the bake, so the mesh enters the import at its last
stage instead of being lit again. Every `--poses`/`--reference` pair trains
one mesh together (the path-averaged mesh); `--per-shot` trains one per
pair, for a mesh swapped as the camera moves through each segment.

The fit runs on a CUDA GPU in its own environment, not
[requirements.txt](requirements.txt). One setup that builds it, on WSL 2
Debian with the Windows NVIDIA driver, no root, a conda environment for
the CUDA 12.8 compiler and a GCC that CUDA 12.8 accepts:

```sh
micromamba create -y -p ~/gpu/env -c nvidia/label/cuda-12.8.1 -c conda-forge \
    python=3.12 cuda-nvcc cuda-cudart-dev cuda-cccl cuda-libraries-dev ninja "gxx_linux-64=13" "gcc_linux-64=13"
E=~/gpu/env
$E/bin/python -m ensurepip
$E/bin/python -m pip install -q --progress-bar off torch==2.11.0 --index-url https://download.pytorch.org/whl/cu128
$E/bin/python -m pip install -q --progress-bar off numpy pillow ninja setuptools wheel
CUDA_HOME=$E PATH=$E/bin:$PATH TORCH_CUDA_ARCH_LIST=12.0 \
    CPATH=$E/targets/x86_64-linux/include LIBRARY_PATH=$E/targets/x86_64-linux/lib:$E/lib \
    CC=$E/bin/x86_64-conda-linux-gnu-gcc CXX=$E/bin/x86_64-conda-linux-gnu-g++ \
    $E/bin/python -m pip install -q --progress-bar off --no-build-isolation git+https://github.com/NVlabs/nvdiffrast.git
```

That gives PyTorch 2.11.0+cu128 and nvdiffrast 0.4.0; `TORCH_CUDA_ARCH_LIST`
names the GPU's compute capability (12.0 is Blackwell). Then, from the
repository root, with the reference images of the training poses:

```sh
$E/bin/python launcher/tools/r3d/appearance_simplify.py --start MESH_mesh_generated.c \
    --poses train.txt --reference reference_train --out fitted --clear RRGGBB
```

To score a fitted mesh, build the scene's host renderer with it in place of
the tracked mesh (`--build-only --substitute TRACKED.c=fitted/MESH_mesh_generated.c`
of the host-render script) and score its video of poses the fit never saw
with `render_compare.py --reference-video`, as in
[Fidelity reference](#fidelity-reference). `test_r3d_appearance.py` runs
the fit itself only where CUDA, PyTorch and nvdiffrast import.

## Cost-aware fit

Three additions spend a triangle budget where the camera looks and weigh
appearance against frame time.

| Stage | What it does | Where |
|---|---|---|
| Path visibility | The import's `process.visibility` with `source = "camera_path"` keeps only source triangles a ray from some pose of the camera's path lands on, before lighting and simplification, so the budget goes to surfaces the path shows | `light.visible_from_path`, [Mesh-Import.md](../../../docs/render/Mesh-Import.md#import-file) |
| Pruning | `--budget N` draws every pose of `--coverage-poses` (the training poses when omitted) and counts the pixels each triangle shows; triangles no pose shows go first, then those showing fewest, down to N. Simplifying to more than N and pruning back puts the triangles where they show | `appearance_simplify.coverage`, `prune` |
| Cost term | `--cost-model WEIGHTS --cost-weight L` adds L dE76 per predicted millisecond to the loss: the model's drawn-triangle, row and pixel terms, differentiable in the vertex positions | `appearance_simplify.predicted_ms`, `cost_model.triangle_terms` |
| Normal term | `--normal-weight L` adds L times the mean L1 distance between the mesh's interpolated vertex normals and the reference's normal buffer (`reference_render.py --normals`, `NNN.normal.npy` beside each image) where both cover a pixel; `--score` fits nothing and prints the start's mean normal angle | `appearance_simplify.normal_l1`, `normal_error` |
| Warm start | `--refine-to N` splits the longest edge of the start's worst triangles, by dE summed over the pixels they show, both sides of an edge at once, until N; a fitted coarse mesh then starts a finer fit | `appearance_simplify.refine`, `tessellate.split_marked_edges` |

The cost model's weights come from board frame times of meshes with
different triangle counts and overdraw:

```sh
$PY tools/r3d/cost_model.py features MESH_mesh_generated.c --poses BOARD_POSES   # one row per pose
$PY tools/r3d/cost_model.py fit TABLE > weights.txt                              # rows of `ms feature...`
$PY tools/r3d/cost_model.py predict MESH_mesh_generated.c --poses BOARD_POSES --weights weights.txt
```

`BOARD_POSES` are the poses the board's frame-cost suite times, at its render
size, so a feature row lines up with a measured frame.

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
