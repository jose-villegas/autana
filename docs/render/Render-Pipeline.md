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
            Cull["Cull"] --> Xf["Transform"] --> Clip["Clip"] --> Fill["Span fill<br/><i>+ attachments</i>"] --> Res["Resolve<br/><i>only if an attachment resolves</i>"] --> View["View<br/>modes"]
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

<!-- generated: bake-machine sha256=23d185bd8bd39958b2336baa15b8d3db3c368ebd0344c0b3af20c58dbc9ec351 -->
| Machine | Value |
|---|---|
| CPU | Intel(R) Core(TM) i7-10750H CPU @ 2.60GHz |
| Cores/threads | 5/10 |
| RAM | 10.7 GiB |
| GPU | NVIDIA GeForce RTX 2060 |
| VRAM | 6144 MiB |
| Driver | 617.14 |
| CUDA | 12.8 |
| OS | Linux-6.18.54.1-microsoft-standard-WSL2-x86_64-with-glibc2.39 |
| Python | 3.12.14 |
| Mitsuba | 3.9.1 |
| PyTorch | 2.11.0+cu128 |
<!-- /generated: bake-machine -->

Each step of those bakes, with the triangles it took and left and the memory
it peaked at:

<!-- generated: bake-steps sha256=debe9fa09d7a1519ec944c50b89f3668698f5fc7094a0f4e34f9bb5eebef30cc -->
| Variant | Step | Wall s | Triangles in | Triangles out | Peak RAM MiB | Peak VRAM MiB |
|---|---|---|---|---|---|---|
| lite-GI-bake | source load | 11.255 | 0 | 262267 | 1555.1 | not available |
| lite-GI-bake | alpha mask | 0.313 | 262267 | 245465 | 1509.6 | not available |
| lite-GI-bake | visibility | 15.696 | 245465 | 211859 | 1906.1 | not available |
| lite-GI-bake | thin | 0.003 | 211859 | 202188 | 1844.0 | not available |
| lite-GI-bake | light | 113.732 | 202188 | 681348 | 3294.2 | not available |
| lite-GI-bake | simplify | 34.405 | 681348 | 8669 | 3435.5 | not available |
| lite-GI-bake | meshlets | 0.061 | 8669 | 8669 | 3225.9 | not available |
| lite-GI-bake | write | 0.002 | 8669 | 8669 | 3225.0 | not available |
| lite-fit-start | source load | 6.324 | 0 | 262267 | 1511.6 | not available |
| lite-fit-start | alpha mask | 0.179 | 262267 | 245465 | 1497.7 | not available |
| lite-fit-start | visibility | 11.168 | 245465 | 211859 | 1903.0 | not available |
| lite-fit-start | thin | 0.002 | 211859 | 202188 | 1833.8 | not available |
| lite-fit-start | light | 46.680 | 202188 | 681348 | 3302.9 | not available |
| lite-fit-start | simplify | 17.619 | 681348 | 13494 | 3427.5 | not available |
| lite-fit-start | meshlets | 0.068 | 13494 | 13494 | 3319.0 | not available |
| lite-fit-start | write | 0.002 | 13494 | 13494 | 3319.0 | not available |
| lite-GI-fit | fit | 922.178 | 13494 | 8672 | 2721.3 | 1138.0 |
| full-GI-bake | source load | 8.393 | 0 | 262267 | 1544.5 | not available |
| full-GI-bake | alpha mask | 0.216 | 262267 | 245465 | 1508.9 | not available |
| full-GI-bake | visibility | 14.750 | 245465 | 211859 | 1905.7 | not available |
| full-GI-bake | thin | 0.005 | 211859 | 202188 | 1823.6 | not available |
| full-GI-bake | light | 81.568 | 202188 | 681348 | 3303.4 | not available |
| full-GI-bake | simplify | 15.045 | 681348 | 17367 | 3430.5 | not available |
| full-GI-bake | meshlets | 0.165 | 17367 | 17367 | 3316.0 | not available |
| full-GI-bake | write | 0.018 | 17367 | 17367 | 3316.0 | not available |
| full-fit-start | source load | 6.658 | 0 | 262267 | 1558.2 | not available |
| full-fit-start | alpha mask | 0.202 | 262267 | 245465 | 1512.7 | not available |
| full-fit-start | visibility | 11.730 | 245465 | 211859 | 1921.4 | not available |
| full-fit-start | thin | 0.003 | 211859 | 202188 | 1823.0 | not available |
| full-fit-start | light | 77.276 | 202188 | 681348 | 3305.2 | not available |
| full-fit-start | simplify | 34.043 | 681348 | 29995 | 3446.4 | not available |
| full-fit-start | meshlets | 0.592 | 29995 | 29995 | 3277.5 | not available |
| full-fit-start | write | 0.013 | 29995 | 29995 | 3277.5 | not available |
| full-GI-fit | fit | 1051.771 | 29995 | 17381 | 2704.2 | 1164.0 |
| flat-GI-bake | source load | 7.975 | 0 | 262267 | 1558.9 | not available |
| flat-GI-bake | alpha mask | 0.341 | 262267 | 245465 | 1501.5 | not available |
| flat-GI-bake | visibility | 14.769 | 245465 | 211859 | 1936.0 | not available |
| flat-GI-bake | thin | 0.002 | 211859 | 202188 | 1828.0 | not available |
| flat-GI-bake | light | 52.463 | 202188 | 681348 | 3293.4 | not available |
| flat-GI-bake | simplify | 10.523 | 681348 | 17367 | 3414.9 | not available |
| flat-GI-bake | face colours | 11.829 | 17367 | 17367 | 3215.5 | not available |
| flat-GI-bake | meshlets | 0.210 | 17367 | 17367 | 3215.6 | not available |
| flat-GI-bake | write | 0.002 | 17367 | 17367 | 3215.6 | not available |
| flat-fit-start | source load | 8.514 | 0 | 262267 | 1551.6 | not available |
| flat-fit-start | alpha mask | 0.204 | 262267 | 245465 | 1511.5 | not available |
| flat-fit-start | visibility | 15.310 | 245465 | 211859 | 1920.2 | not available |
| flat-fit-start | thin | 0.003 | 211859 | 202188 | 1837.8 | not available |
| flat-fit-start | light | 69.413 | 202188 | 681348 | 3304.0 | not available |
| flat-fit-start | simplify | 13.745 | 681348 | 29995 | 3427.3 | not available |
| flat-fit-start | face colours | 21.977 | 29995 | 29995 | 3261.8 | not available |
| flat-fit-start | meshlets | 0.378 | 29995 | 29995 | 3258.1 | not available |
| flat-fit-start | write | 0.005 | 29995 | 29995 | 3258.1 | not available |
| flat-GI-fit | fit | 779.912 | 29995 | 17381 | 2703.7 | 1200.0 |
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
path-traced bounced light. Rays are traced by Mitsuba<sup>[[15]](../Citations.md#15)</sup> on the CPU (its LLVM
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
the camera's poses<sup>[[1]](../Citations.md#1)</sup>.
It needs an NVIDIA GPU with CUDA: PyTorch and nvdiffrast<sup>[[2]](../Citations.md#2)</sup> draw what the board
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
| the final mesh | clusters, nodes, the `.mesh` entry | `geometry.meshlet_triangles` |

Cost: `meshlets` and `write` in the bake-steps table.

The meshlets debug view gives each cluster a flat hue, shown here for the
full, lite and fitted meshes. Each instance owns a disjoint ID range, reset on
every draw; empty pixels keep the clear colour.

![Clusters painted by the meshlets debug view](../images/render/sponza-meshlets.png)

Culling clusters about halves the triangles submitted and takes roughly a
third off the frame. Meshlets of 16 and 32 tie; 64 loses, because its coarser
boxes let through more triangles than its fewer culls save. Each size on the
board, against submitting every cluster:

<!-- generated: meshlet-sizes sha256=c7e540ba268898185d69761faec683daf36b2d7dbdb1dd190417814a1b0d54a8 -->
| Row | Clusters | Vertices/triangle | Mean submitted triangles | Cull ms | Transform ms | Draw ms | Frame ms |
|---|---|---|---|---|---|---|---|
| cull off | 605 | 1.040 | 16929.0 | 0.34 | 5.70 | 48.46 | 57.28 |
| meshlets of 16 | 1290 | 1.166 | 8082.3 | 0.94 | 3.62 | 32.98 | 40.36 |
| meshlets of 32 (shipped) | 605 | 1.040 | 8652.7 | 0.60 | 3.24 | 33.89 | 40.57 |
| meshlets of 64 | 296 | 0.959 | 9529.5 | 0.43 | 3.17 | 35.45 | 41.67 |
<!-- /generated: meshlet-sizes -->

Reference: [Baked-Mesh.md](Baked-Mesh.md#meshlets).

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

Separate charts show the shipped frame and each debug view. The shipped
chart uses solid segments; each view chart shows hatched additions, with an
label and elbow leader marking the base frame at the bar's start. A segment
is drawn only when its share rounds above zero percent. Legends match the
drawn segments; the table retains every stage in milliseconds. Each axis fits
its own data, and view ticks show added milliseconds.
The shipped label gives the total; view labels give the signed net addition
and its share of the shipped total. Segment labels and legends give each
stage's share of the shipped total, signed for view additions. Dotted lines
mark the frame-rate budgets when they fall inside the chart.

<!-- generated: pipeline-frame-stages sha256=4ca11d90dd2b848aa4842035ac3193109d3428e9c6ec73ccb768b6904f650410 -->
![Frame stages as shipped](../images/render/pipeline-frame-stages.png)

![Frame stages with the depth view](../images/render/pipeline-frame-stages.depth.png)

![Frame stages with the tiles view](../images/render/pipeline-frame-stages.tiles.png)

![Frame stages with the motion view](../images/render/pipeline-frame-stages.motion.png)

![Frame stages with the meshlets view](../images/render/pipeline-frame-stages.meshlets.png)

| Stage | As shipped | Share | depth view adds | tiles view adds | motion view adds | meshlets view adds |
|---|---|---|---|---|---|---|
| r3d.cull | 0.76 | 1% | +0.01 | +0.00 | +0.02 | +0.01 |
| r3d.transform | 4.87 | 10% | +0.10 | +0.02 | +0.22 | +0.02 |
| r3d.draw | 41.94 | 82% | +0.25 | +0.20 | +1.85 | +23.89 |
| r3d.resolve | not run | not run | +0.00 | +0.00 | +14.29 | +0.00 |
| r3d.show | not run | not run | +6.89 | +4.60 | +9.05 | +10.21 |
| present.wait | 0.00 | 0% | +0.00 | +0.00 | +0.00 | +0.00 |
| r3d.upscale | 2.59 | 5% | +0.16 | +0.17 | +0.16 | +0.16 |
| frame.rest | 1.07 | 2% | +0.06 | +0.05 | +0.11 | +0.12 |
| total | 51.23 | 100% | +7.47 | +5.04 | +25.70 | +34.40 |

Board ms per frame, mean of 78 to 79 report windows per capture; a view adds the difference between its capture and the scene as shipped.
<!-- /generated: pipeline-frame-stages -->

### Scene and camera

The scene manager moves the active camera along its path and hands the render
context the scene's instances, a `render_view_t` and a clear colour; how the
view is built is in [Mesh-Rendering.md](Mesh-Rendering.md#what-a-scene-uses).

| Reads | Writes | Settings |
|---|---|---|
| the scene's pack: meshes, placements, the camera clip | the frame's instances and view | the active camera, which renderers are enabled |

Cost: part of `frame.rest` in the frame-stages chart.

![The demo scene along its flythrough](../images/render/sponza-fitted-full.gif)

Reference: [Scene-Manager.md](Scene-Manager.md#each-frame).

### Render size

The render context picks the size the frame is drawn at: a fixed share of the
panel, or a ladder step chosen to hold a frame budget, from recent frames'
cost or, predicted, from what the census kept.

| Reads | Writes | Settings |
|---|---|---|
| recent frames' cost, or the census's triangles | the render width and height | the scale, or the budget, ladder and policy |

Cost: part of `frame.rest` in the frame-stages chart.

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

Cost: `r3d.cull` in the frame-stages chart; a predicted size is charged to
`r3d.census` instead.

Picture: none yet.

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#one-frame).

#### Transform

Each vertex of the kept clusters is projected once, the two cores taking
halves of the vertex work; each cluster also records the screen rows it spans.

| Reads | Writes | Settings |
|---|---|---|
| the kept clusters, positions, the lens fitted to the render size | screen vertices, each cluster's rows | none |

Cost: `r3d.transform` in the frame-stages chart.

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#on-both-cores).

#### Clip

Inside the draw, a triangle with a corner behind the near plane or far off
screen is rebuilt and clipped; the rest pass straight on. Its cost is part of
the draw's.

| Reads | Writes | Settings |
|---|---|---|
| screen vertices | clipped triangles | none |

Cost: part of `r3d.draw` in the frame-stages chart.

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#coverage-and-small-triangles).

#### Span fill

Each triangle is filled span by span into colour and depth by the top-left
rule; after each span, every attachment's writer marks the pixels that
triangle won. The two cores split the rows where the estimated cost halves.

| Reads | Writes | Settings |
|---|---|---|
| triangles and their colours | colour, depth, each attachment's pixels | the attachments listed |

Cost: `r3d.draw` in the frame-stages chart.

![The depth the fill writes](../images/render/sponza-depth.gif)

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#coverage-and-small-triangles).

#### Resolve

Once every instance is drawn, each attachment that resolves turns what was
written into its final map, the two cores on disjoint rows.

| Reads | Writes | Settings |
|---|---|---|
| depth, the attachment's pixels | the attachment's map | none |

Cost: `r3d.resolve` in the frame-stages chart, only when a view's attachment
resolves; the scene as shipped has none.

![Motion vectors, an attachment resolved from depth](../images/render/sponza-motion-vectors.gif)

Reference: [Mesh-Rendering.md](Mesh-Rendering.md#attachments).

#### View modes

Development builds select depth, tiles, motion or meshlets from the declared
view table in `render/context/render_context.c`. Before upscale, the selected
attachment paints colour from the renderer's maps.

| Reads | Writes | Settings |
|---|---|---|
| depth, an attachment | colour | the debug view |

Cost: `r3d.show` in each view's chart.

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
and column maps, the two cores taking halves.

A frame whose width is exactly half and whose height divides the half height
expands into a half picture, which present doubles as it sends. A camera's
present mode uses this to draw half the rows
([camera presentation](Dynamic-Resolution.md#camera-presentation)).

| Reads | Writes | Settings |
|---|---|---|
| the picture | the framebuffer, or a half picture | the render size against the panel |

Cost: `r3d.upscale` in the frame-stages chart.

Reference: [gfx/Targets-and-Modes.md](../gfx/Targets-and-Modes.md#expanded-frames).

#### Present

The framebuffer's dirty rows, or a half picture doubled on the way, go to the
panel over QSPI while the next frame is prepared; the frame-stages chart shows
what the frame waits for it.

| Reads | Writes | Settings |
|---|---|---|
| the framebuffer or half picture, its dirty rows | the panel | the framebuffer layout (`gfx_mode.h`) |

Cost: `present.wait` in the frame-stages chart.

Reference: [gfx/Present.md](../gfx/Present.md#present-what-gets-sent).

## Refreshing

The docs generator, `launcher/tools/render/render_doc_images.sh`, refreshes
this page's measured blocks from the committed captures and GPU stage.

The generator writes the `pipeline-frame-stages` chart and table from
frame-cost report windows: every bracket of the scene as shipped, and for each
debug view in the view table of `render/context/render_context.c`, what its
capture adds to each bracket. Capture them all from one development build
running the demo scene, the first with the scene as shipped, then one per view.
Two minutes spans several passes of the camera path, so where along the path a
capture starts barely moves its means:

```sh
autana monitor 120 --out docs/render/data/pipeline-present-board.log
autana monitor 120 --out docs/render/data/pipeline-view-motion-board.log
```

The debug view is a development-build tunable (`autana tune view` lists it):
0 (`RENDER_DEBUG_VIEW_SHADED` in `render/context/render_context.h`) is the
scene as shipped, and each view is its place in the view table,
counted from 1. Each view's capture is `pipeline-view-<name>-board.log`, and a
view without one is listed as not captured. Check `autana status` and `autana buildid`
before and after each capture. A missing capture of the scene as shipped, or a
capture without report windows, fails the run.

The `meshlet-sizes` table is generated from each size's own suite capture and
regenerated bake. The [meshlet capture recipe](../../launcher/tools/render/README.md#meshlet-size-captures)
owns preparation, capture validation and refresh commands.

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
