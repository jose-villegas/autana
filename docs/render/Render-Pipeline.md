# Render Pipeline

A 3D frame on the board starts on the host: a model is baked into meshes the
board draws without lighting them. This page follows that path in order, from
the source model to the panel, one short section per stage. Each says what the
stage reads and writes, its settings, its cost and what it looks like, and
links to the page that documents it in full. Every number here, and every
image under `docs/images/`, is generated from the source by the doc-images pipeline
([Render-Harness.md](../tools/Render-Harness.md#images-in-these-docs)), on the
demo scene in the [demo assets](../../launcher/demo/README.md);
[Refreshing](#refreshing) says how to take new board captures and rerun it.

| [Offline, on the host](#offline-on-the-host) | [Every frame, on the board](#every-frame-on-the-board) |
|---|---|
| **[Geometry](#geometry)**<br/>↳ [Source model](#source-model) → [Alpha mask](#alpha-mask) → [Visibility](#visibility) → [Thin](#thin) | **[Scene and camera](#scene-and-camera)** |
| **[Light](#light)** | **[Render size](#render-size)** |
| **[Shape](#shape)**<br/>↳ [Simplify](#simplify) → [Fit](#fit) | **[Update](#update)**<br/>↳ [Cull](#cull) → [Transform](#transform) → [Clip](#clip) → [Span fill](#span-fill) → [Resolve](#resolve) → [View modes](#view-modes) |
| **[Output](#output)**<br/>↳ [Meshlets](#meshlets) → [Asset pack](#asset-pack) | **[After the send](#after-the-send)**<br/>↳ [Compose](#compose) → [Present](#present) |

```mermaid
flowchart TB
    subgraph Host["Offline, on the host: tools/r3d"]
        direction TB
        subgraph Geometry["Geometry"]
            direction LR
            Src["Source<br/>model"] --> Mask["Alpha<br/>mask"] --> Vis["Visibility"] --> Thin["Thin"]
        end
        Light["Light<br/><i>direct · occlusion · bounce</i>"]
        subgraph Shape["Shape"]
            direction LR
            Simp["Simplify"] --> Fit["Fit<br/><i>smooth or flat, CUDA</i>"]
        end
        subgraph Output["Output"]
            direction LR
            Mesh["Meshlets"] --> Pack["Asset pack"]
        end
        Geometry --> Light --> Shape --> Output
    end
    subgraph Board["Every frame, on the board"]
        direction TB
        Scene["scene/<br/><i>scene and camera</i>"] --> Size["render/context<br/><i>render size</i>"]
        subgraph Update["Update while the last frame is sent: render/raster, both cores"]
            direction LR
            Cull["Cull"] --> Xf["Transform"] --> Clip["Clip"] --> Fill["Span fill<br/><i>+ attachments</i>"] --> Res["Resolve"] --> View["View<br/>modes"]
        end
        subgraph After["After the send"]
            direction LR
            Up["Compose: upscale,<br/><i>or copy into a half picture</i>"] --> Present["gfx/present<br/><i>dirty rows, or a half<br/>picture doubled</i>"]
        end
        Size --> Update
        Update -. "predicted: census first" .-> Size
        Update --> After
    end
    Output --> Board
```

## Offline, on the host

Every step runs on the CPU except the fit, which needs a CUDA GPU.
The doc-images GPU stage rebakes the demo scene's meshes on the machine in
the table below:

<!-- generated: bake-machine sha256=22db5697ef2f286e26c3119383781e5ee8b7157c6eccd77e726db8e96c916eb6 -->
| Machine | Value |
|---|---|
| CPU | Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz |
| Cores/threads | 5/10 |
| RAM | 10.7 GiB |
| GPU | NVIDIA GeForce RTX 2060 |
| VRAM | 6144 MiB |
| Driver | 617.14 |
| CUDA | 12.8 |
| OS | Linux-6.18.40.1-microsoft-standard-WSL2-x86_64-with-glibc2.39 |
| Python | 3.12.14 |
| Mitsuba | 3.9.1 |
| PyTorch | 2.11.0+cu128 |
<!-- /generated: bake-machine -->

Each step of those bakes, with the triangles it took and left and the memory
it peaked at:

<!-- generated: bake-steps sha256=1862d81d84c3f4457b838958783d7618fef40f7390dda7c6f2d892e65022ef9a -->
| Variant | Step | Wall s | Triangles in | Triangles out | Peak RAM MiB | Peak VRAM MiB |
|---|---|---|---|---|---|---|
| lite-GI-bake | source load | 6.942 | 0 | 262267 | 1521.9 | not available |
| lite-GI-bake | alpha mask | 0.192 | 262267 | 245465 | 1496.5 | not available |
| lite-GI-bake | visibility | 12.497 | 245465 | 211859 | 1804.4 | not available |
| lite-GI-bake | thin | 0.006 | 211859 | 202188 | 1764.9 | not available |
| lite-GI-bake | light | 84.111 | 202188 | 681348 | 3336.2 | not available |
| lite-GI-bake | simplify | 14.744 | 681348 | 8669 | 3457.7 | not available |
| lite-GI-bake | meshlets | 0.043 | 8669 | 8669 | 3302.9 | not available |
| lite-GI-bake | write | 0.001 | 8669 | 8669 | 3300.0 | not available |
| lite-fit-start | source load | 5.331 | 0 | 262267 | 1518.8 | not available |
| lite-fit-start | alpha mask | 0.157 | 262267 | 245465 | 1501.3 | not available |
| lite-fit-start | visibility | 461.196 | 245465 | 120792 | 2477.9 | not available |
| lite-fit-start | thin | 0.002 | 120792 | 114665 | 2110.6 | not available |
| lite-fit-start | light | 30.466 | 114665 | 288503 | 3445.0 | not available |
| lite-fit-start | simplify | 11.870 | 288503 | 9972 | 3455.3 | not available |
| lite-fit-start | meshlets | 0.039 | 9972 | 9972 | 3446.9 | not available |
| lite-fit-start | write | 0.001 | 9972 | 9972 | 3446.0 | not available |
| lite-GI-fit | fit | 318.465 | 9972 | 8672 | 1737.5 | 546.0 |
| full-GI-bake | source load | 6.074 | 0 | 262267 | 1510.4 | not available |
| full-GI-bake | alpha mask | 0.184 | 262267 | 245465 | 1496.7 | not available |
| full-GI-bake | visibility | 10.854 | 245465 | 211859 | 1806.6 | not available |
| full-GI-bake | thin | 0.003 | 211859 | 202188 | 1777.2 | not available |
| full-GI-bake | light | 57.997 | 202188 | 681348 | 3333.6 | not available |
| full-GI-bake | simplify | 9.615 | 681348 | 17367 | 3459.1 | not available |
| full-GI-bake | meshlets | 0.062 | 17367 | 17367 | 3281.1 | not available |
| full-GI-bake | write | 0.002 | 17367 | 17367 | 3281.1 | not available |
| full-fit-start | source load | 7.370 | 0 | 262267 | 1558.0 | not available |
| full-fit-start | alpha mask | 0.241 | 262267 | 245465 | 1512.6 | not available |
| full-fit-start | visibility | 733.965 | 245465 | 120792 | 2506.1 | not available |
| full-fit-start | thin | 0.006 | 120792 | 114665 | 2080.4 | not available |
| full-fit-start | light | 50.744 | 114665 | 288503 | 3403.2 | not available |
| full-fit-start | simplify | 6.690 | 288503 | 19987 | 3410.4 | not available |
| full-fit-start | meshlets | 0.101 | 19987 | 19987 | 3403.7 | not available |
| full-fit-start | write | 0.012 | 19987 | 19987 | 3402.8 | not available |
| full-GI-fit | fit | 324.540 | 19987 | 17177 | 1745.0 | 560.0 |
| flat-GI-bake | source load | 5.479 | 0 | 262267 | 1523.6 | not available |
| flat-GI-bake | alpha mask | 0.152 | 262267 | 245465 | 1498.2 | not available |
| flat-GI-bake | visibility | 9.236 | 245465 | 211859 | 1805.6 | not available |
| flat-GI-bake | thin | 0.002 | 211859 | 202188 | 1772.5 | not available |
| flat-GI-bake | light | 55.080 | 202188 | 681348 | 3331.3 | not available |
| flat-GI-bake | simplify | 9.294 | 681348 | 17367 | 3455.1 | not available |
| flat-GI-bake | face colours | 9.201 | 17367 | 17367 | 3303.0 | not available |
| flat-GI-bake | meshlets | 0.148 | 17367 | 17367 | 3302.2 | not available |
| flat-GI-bake | write | 0.002 | 17367 | 17367 | 3298.3 | not available |
| flat-fit-start | source load | 6.544 | 0 | 262267 | 1558.4 | not available |
| flat-fit-start | alpha mask | 0.166 | 262267 | 245465 | 1500.9 | not available |
| flat-fit-start | visibility | 586.139 | 245465 | 120792 | 2491.8 | not available |
| flat-fit-start | thin | 0.002 | 120792 | 114665 | 2174.6 | not available |
| flat-fit-start | light | 30.009 | 114665 | 288503 | 3498.5 | not available |
| flat-fit-start | simplify | 4.580 | 288503 | 19987 | 3511.4 | not available |
| flat-fit-start | face colours | 9.083 | 19987 | 19987 | 3498.8 | not available |
| flat-fit-start | meshlets | 0.146 | 19987 | 19987 | 3498.9 | not available |
| flat-fit-start | write | 0.002 | 19987 | 19987 | 3498.9 | not available |
| flat-GI-fit | fit | 300.213 | 19987 | 17177 | 1746.4 | 588.0 |
<!-- /generated: bake-steps -->

### Geometry

These stages select the triangles carried into the light bake.

#### Source model

The import file names an OBJ with its materials and textures; the importer
loads it whole.

| Reads | Writes | Settings |
|---|---|---|
| OBJ, MTL, textures | triangles with materials and UVs | `[source]` path and credit |

Cost: `source load` in the bake-steps table.

The source lit per pixel is the reference every bake is scored against (left,
in the [fidelity sheet](Bake-Quality.md#fidelity-against-the-source)):

![The source lit per pixel beside the flat bake](../images/render/bake-fidelity-sheet.png)

Reference: [Mesh-Import.md](Mesh-Import.md#source).

#### Alpha mask

Drops alpha-tested triangles that are mostly transparent; the board does not alpha-test.

| Reads | Writes | Settings |
|---|---|---|
| triangles, textures | fewer triangles | `geometry.alpha_mask.keep_alpha` |

Cost: `alpha mask` in the bake-steps table.

![Triangles kept and dropped by the alpha mask](images/import-alpha-mask.png)

Reference: [Mesh-Import.md](Mesh-Import.md#alpha_mask).

#### Visibility

Keeps only the triangles the camera can see, from anywhere in its region or
along its path, so the triangle budget goes to what is drawn.

| Reads | Writes | Settings |
|---|---|---|
| triangles, the camera's region or path | the seen triangles | the renderer's `visibility` |

Cost: `visibility` in the bake-steps table.

![Triangles the camera region sees](images/import-visibility.png)

Reference: [Scene-Files.md](Scene-Files.md#visibility-camera_region).

#### Thin

Keeps a random share of one material's seen triangles, for foliage too dense
to draw whole. It runs after visibility so the share is of what is seen.

| Reads | Writes | Settings |
|---|---|---|
| the seen triangles | fewer of that material's triangles | `geometry.thin.material`, `geometry.thin.keep` |

Cost: `thin` in the bake-steps table.

![A thinned material](images/import-thin.png)

Reference: [Mesh-Import.md](Mesh-Import.md#thin).

### Light

Bakes each vertex's colour: direct sun and sky, local occlusion, and
path-traced bounced light. Rays are traced by Mitsuba on the CPU (its LLVM
backend).

| Reads | Writes | Settings |
|---|---|---|
| dense triangles, materials, the scene's lights | a colour per vertex | `[bake]` with `ao` and `indirect`, `[indirect]`, sky, ambient, tone map |

Cost: `light`, and `face colours` for flat meshes, in the bake-steps table.

![Albedo against baked light](../images/render/import-light.png)

What occlusion and bounced light buy is measured in
[Bake-Quality.md](Bake-Quality.md#indirect-light). Reference:
[Scene-Files.md](Scene-Files.md#bake).

### Shape

These stages reduce and fit the lit mesh for the board.

#### Simplify

Reduces each variant to its triangle budget, weighing the baked colours per
part, with a share kept for small props and touching pieces joined first so
no seam opens.

| Reads | Writes | Settings |
|---|---|---|
| lit triangles | the variant's mesh | `geometry.simplify`: `dense_edge`, `props`, `props_share`, `seal_seams`, `colour_deviation`; each variant's `triangles` |

Cost: `simplify` in the bake-steps table.

![A seam left open and sealed](images/import-seal-seams.png)

Reference: [Mesh-Import.md](Mesh-Import.md#simplify).

#### Fit

Optional: moves a variant's vertices and colours to match the reference from
the camera's poses.
It needs an NVIDIA GPU with CUDA: PyTorch and nvdiffrast draw what the board
draws and fit against the reference; see the
[fit environment](../../launcher/tools/r3d/README.md#appearance-fit). A smooth mesh fits colours per vertex, a flat one a colour
per face.

| Reads | Writes | Settings |
|---|---|---|
| a variant, reference renders | the fitted mesh | the renderer's `fit`: prune, poses, optimise, hashes |

Cost: `fit` in the bake-steps table.

![A fitted mesh against the reference](../images/render/appearance-fitted-full-reference.png)

Reference: [Scene-Files.md](Scene-Files.md#fitprune); measured in
[Bake-Quality.md](Bake-Quality.md#appearance-fit-of-the-lite-and-full-meshes).

### Output

These stages package the mesh for loading and drawing on the board.

#### Meshlets

Cuts the mesh into clusters under a tree of bounding nodes, the units the board
culls and draws.

| Reads | Writes | Settings |
|---|---|---|
| the final mesh | clusters, nodes, the `.mesh` entry | none |

Cost: `meshlets` and `write` in the bake-steps table.

The meshlets debug view gives each cluster a flat hue, shown here for the
full, lite and fitted meshes. Each instance owns a disjoint ID range, reset on
every draw; empty pixels keep the clear colour.

![Clusters painted by the meshlets debug view](../images/render/sponza-meshlets.png)

Reference: [Mesh-Import.md](Mesh-Import.md#meshlets).

#### Asset pack

Packs each root (a scene with its meshes and camera clip, or a lone import or
clip) into one pack; `launcher/demo/` content ships only where an app's
`demo_assets.toml` names it.

| Reads | Writes | Settings |
|---|---|---|
| the scene, its meshes and clip | one pack per root, the partition image | an app's list of demo assets |

Cost: not timed in the bake-steps table.

Reference: [the asset packs](../assets/README.md#packs).

## Every frame, on the board

Each stage of the scene as shipped:

<!-- generated: pipeline-frame-stages sha256=866982c25d43099be4e7a5d0afac643e454c551f6de1930496a3590a742e50c3 -->
| Stage | Board ms/frame |
|---|---|
| r3d.cull | 0.72 |
| r3d.transform | 6.61 |
| r3d.draw | 40.79 |
| r3d.resolve, with motion vectors attached | 14.84 |
| present.wait | 0.00 |
| r3d.upscale | 2.88 |
| frame.rest | 1.13 |

Source: the scene as shipped, mean of 20 windows; the resolve row is from the scene with the motion attachment shown.
<!-- /generated: pipeline-frame-stages -->

### Scene and camera

The scene manager moves the active camera along its path and hands the render
context the scene's instances, a `render_view_t` and a clear colour; how the
view is built is in [Mesh-Rendering.md](Mesh-Rendering.md#what-a-scene-uses).

| Reads | Writes | Settings |
|---|---|---|
| the scene's pack: meshes, placements, the camera clip | the frame's instances and view | the active camera, which renderers are enabled |

Cost: part of `frame.rest` in the frame-stages table.

![The demo scene along its flythrough](../images/render/sponza-full.gif)

Reference: [Scene-Manager.md](Scene-Manager.md#each-frame).

### Render size

The render context picks the size the frame is drawn at: a fixed share of the
panel, or a ladder step chosen to hold a frame budget, from recent frames'
cost or, predicted, from what the census kept.

| Reads | Writes | Settings |
|---|---|---|
| recent frames' cost, or the census's triangles | the render width and height | the scale, or the budget, ladder and policy |

Cost: part of `frame.rest` in the frame-stages table.

![The size chosen along the path](../images/render/dynamic-resolution-flight.png)

Reference: [Dynamic-Resolution.md](Dynamic-Resolution.md).

### Update

These stages run on both cores while the last frame is sent.

#### Cull

The cull walks each mesh's node tree against the camera's frustum, roughly
nearest first, and keeps the clusters that may be on screen. At a fixed or stepped
size the draw culls each instance just before drawing it; when the size is
predicted, the census culls every instance first, the size is priced from its
list, and the draw reuses it.

| Reads | Writes | Settings |
|---|---|---|
| node and cluster bounds, the camera | the kept clusters | none |

Cost: `r3d.cull` in the frame-stages table; a predicted size is charged to
`r3d.census` instead.

Picture: none yet.

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#one-frame).

#### Transform

Each vertex of the kept clusters is projected once, the two cores taking
halves of the vertex work; each cluster also records the screen rows it spans.

| Reads | Writes | Settings |
|---|---|---|
| the kept clusters, positions, the lens fitted to the render size | screen vertices, each cluster's rows | none |

Cost: `r3d.transform` in the frame-stages table.

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#on-both-cores).

#### Clip

Inside the draw, a triangle with a corner behind the near plane or far off
screen is rebuilt and clipped; the rest pass straight on. Its cost is part of
the draw's.

| Reads | Writes | Settings |
|---|---|---|
| screen vertices | clipped triangles | none |

Cost: part of `r3d.draw` in the frame-stages table.

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#coverage-and-small-triangles).

#### Span fill

Each triangle is filled span by span into colour and depth by the top-left
rule; after each span, every attachment's writer marks the pixels that
triangle won. The two cores split the rows where the estimated cost halves.

| Reads | Writes | Settings |
|---|---|---|
| triangles and their colours | colour, depth, each attachment's pixels | the attachments listed |

Cost: `r3d.draw` in the frame-stages table.

![The depth the fill writes](../images/render/sponza-depth.gif)

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#coverage-and-small-triangles).

#### Resolve

Once every instance is drawn, each attachment turns what was written into its
final map, the two cores on disjoint rows.

| Reads | Writes | Settings |
|---|---|---|
| depth, the attachment's pixels | the attachment's map | none |

Cost: `r3d.resolve, with motion vectors attached` in the frame-stages table.

![Motion vectors, an attachment resolved from depth](../images/render/sponza-motion-vectors.gif)

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#attachments).

#### View modes

Development builds select depth, tiles, motion or meshlets from the declared
view table in `render/context/render_context.c`. Before upscale, the selected
attachment paints colour from the renderer's maps.

| Reads | Writes | Settings |
|---|---|---|
| depth, an attachment | colour | the debug view |

Cost: part of `frame.rest` in the frame-stages table when enabled.

Pictures: [depth](../images/render/sponza-depth.gif),
[tiles](../images/render/sponza-tiles.gif),
[motion](../images/render/sponza-motion-vectors.gif),
[meshlets](#meshlets).

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#view-modes).

### After the send

These stages wait for the last frame's send to finish.

#### Compose

After the frame waits for the previous send, the render context picks a half
picture or the panel's framebuffer; raster fills it through precomputed row
and column maps, the two cores taking halves. A frame drawn at exactly half size
is copied into a half picture instead, and the present doubles it as it sends.

| Reads | Writes | Settings |
|---|---|---|
| the picture | the framebuffer, or a half picture | the render size against the panel |

Cost: `r3d.upscale` in the frame-stages table.

Reference: [Gfx-and-Presentation.md](../Gfx-and-Presentation.md#expanded-frames).

#### Present

The framebuffer's dirty rows, or a half picture doubled on the way, go to the
panel over QSPI while the next frame is prepared; the frame-stages table shows
what the frame waits for it.

| Reads | Writes | Settings |
|---|---|---|
| the framebuffer or half picture, its dirty rows | the panel | the framebuffer layout (`gfx_mode.h`) |

Cost: `present.wait` in the frame-stages table.

Reference: [Gfx-and-Presentation.md](../Gfx-and-Presentation.md#present-what-gets-sent).

## Refreshing

The docs generator, `launcher/tools/render/render_doc_images.sh`, refreshes
this page's measured blocks from the committed captures and GPU stage.

The generator writes the `pipeline-frame-stages` table from frame-cost
report windows: every bracket of the scene as shipped, plus the resolve row
from a capture with motion vectors attached. Capture both from the same build:

```sh
autana monitor 30 --out docs/render/data/pipeline-present-board.log
autana monitor 30 --out docs/render/data/pipeline-resolve-board.log
```

The first runs with the scene as shipped; the second with the scene's debug
view set to the motion attachment (a development-build tunable; `autana tune`
lists it). Check `autana status` and `autana buildid` before and after each
capture. A missing capture leaves its row `not in capture`; a capture without
report windows fails the run.

The GPU stage writes the `bake-machine` and `bake-steps` tables from its own
rebakes and fits. Refresh them on the GPU runner with:

```sh
sh launcher/tools/render/render_doc_images.sh --stage gpu
```

RAM peaks are sampled process RSS; GPU steps also read GPU
resident memory and PyTorch's reserved-memory peak. CPU steps have no VRAM
measurement. Each fitted input's rebake is labelled `fit-start`, and its fit
has a separate row. The machine table reads the runner's hardware and
software versions during that run.
