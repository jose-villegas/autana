# 3D Rendering and Scenes

Everything about drawing 3D on the board: importing a model, describing a scene
of objects, and the runtime that draws it. The renderer is `launcher/main/render/`;
the offline tools are `launcher/tools/r3d/`. There is no GPU and no display
framework, only a span rasterizer on both cores.

The whole path, from a source model to the panel, one section per stage, is
the [Render Pipeline](Render-Pipeline.md). To put a model on screen, start
with [Building a Scene](Building-a-Scene.md); to choose how it is drawn, see
[Rendering Options](Rendering-Options.md).

Reference content (a scene with its import, bakes and camera path) lives in
the [demo assets](../../launcher/demo/README.md).

| | |
|---|---|
| [Rendering-Options.md](Rendering-Options.md) | **Choosing options**: every framebuffer layout, present policy and r3d knob that trades work for speed or look, what each costs and when to use it, linked to its reference page. |
| [Render-Pipeline.md](Render-Pipeline.md) | **The overview**: every stage from the source model to the panel in order, what each reads, writes and costs, with a picture and a link to its reference page. |
| [Building-a-Scene.md](Building-a-Scene.md) | **Start here**: import a mesh, place it in a scene, light it, add a camera, draw it. |
| [Scene-Files.md](Scene-Files.md) | The scene file: objects with a transform and one component (mesh renderer, skinned renderer, light, camera), what a scene must carry, and every option, grouped per component. Its pack entry is [SCNE](../assets/formats/SCNE.md). |
| [Scene-Manager.md](Scene-Manager.md) | The runtime side of a scene: load by name, the packed component arrays, the one active camera, what the shell does each frame and who owns what. |
| [Camera.md](Camera.md) | A scene camera: the pose on the entity, the lens beside it, how a view is built, and which lens fields animate. |
| [Mesh-Import.md](Mesh-Import.md) | The import file and the bake stages: opt-in processing steps, simplify, light, and the offline tools. |
| [Appearance-Fit.md](Appearance-Fit.md) | Fitting a baked mesh to reference renders of the source: what each `fit` key does, the stages, the loss, the flat fit, and how it differs from the paper. |
| [Baked-Mesh.md](Baked-Mesh.md) | The baked mesh a renderer consumes: clusters and the tree, the `LMSH` entry layout, meshlets. |
| [Bake-Method.md](Bake-Method.md) | How a bake's light is computed and scored: local occlusion, bounced light, the fidelity numbers. |
| [Bake-Quality.md](Bake-Quality.md) | What each bake option buys, measured against the source: the variants, the appearance fit, indirect light, local occlusion and their board cost. |
| [Skinned-Meshes.md](Skinned-Meshes.md) | **Animated characters**: import a rigged model with its clips, the skin options, the animator and skinned-renderer calls, the board budget and current limits. |
| [Skinned-Lighting.md](Skinned-Lighting.md) | Lighting a skinned mesh every frame: direct N.L against a per-object lookup table, measured, and the recommended path. |
| [Mesh-Rendering.md](Mesh-Rendering.md) | The runtime: cameras, culling, the span rasterizer, instances at transforms, the two-core frame and the view modes. |
| [Dynamic-Resolution.md](Dynamic-Resolution.md) | The render size picked per frame to hold a budget: where a frame's time goes at each size, the stepped controller and the predictor, and how they flew the test path on the board. |
| [Frame-Generation.md](Frame-Generation.md) | Frames warped from the last render to the next camera pose and shown between renders: picture quality on the host, cost on the board, how it prices into dynamic resolution, and the per-context calls an app makes. |

Related: [Animation Tracks](Animation-Tracks.md) for the camera path,
[Firmware](../firmware/README.md) for the layers, and the
[render harness](../tools/Render-Harness.md) to see a scene without a board.
