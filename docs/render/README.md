# 3D Rendering and Scenes

Everything about drawing 3D on the board: importing a model, describing a scene
of objects, and the runtime that draws it. The renderer is `launcher/main/render/`;
the offline tools are `launcher/tools/r3d/`. There is no GPU and no display
framework, only a span rasterizer on both cores.

To put a model on screen, start with [Building a Scene](Building-a-Scene.md).

The [demo assets](../../launcher/demo/README.md) hold reference content;
the Sponza scene, imports, bakes and flythrough live in `launcher/demo/sponza/`.

| | |
|---|---|
| [Building-a-Scene.md](Building-a-Scene.md) | **Start here**: import a mesh, place it in a scene, light it, add a camera, draw it. |
| [Scene-Files.md](Scene-Files.md) | The scene file: objects with a transform and one component (mesh renderer, directional light, camera), sky and ambient, what a scene must carry, and the scene entry (`SCNE`) the pack build bakes from it. |
| [Scene-Manager.md](Scene-Manager.md) | The runtime side of a scene: load by name, the packed component arrays, the one active camera, what the shell does each frame and who owns what. |
| [Mesh-Import.md](Mesh-Import.md) | The import file and the bake stages: opt-in processing steps, simplify, light, meshlets, and the baked format a renderer consumes. |
| [Bake-Quality.md](Bake-Quality.md) | What each bake option buys, measured against the source: the variants, the appearance fit, indirect light, local occlusion and their board cost. |
| [Skinned-Lighting.md](Skinned-Lighting.md) | Lighting a skinned mesh every frame: direct N.L against a per-object lookup table, measured, and the recommended path. |
| [Mesh-Rendering.md](Mesh-Rendering.md) | The runtime: cameras, culling, the span rasterizer, instances at transforms, the two-core frame and the view modes. |
| [Dynamic-Resolution.md](Dynamic-Resolution.md) | The render size picked per frame to hold a budget: where a frame's time goes at each size, the stepped controller and the predictor, and how they flew the test path on the board. |

Related: [Animation Tracks](../Animation-Tracks.md) for the camera path,
[Firmware Architecture](../Firmware-Architecture.md) for the layers, and the
[render harness](../tools/Render-Harness.md) to see a scene without a board.
