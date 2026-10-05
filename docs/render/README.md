# 3D Rendering and Scenes

Everything about drawing 3D on the board: importing a model, describing a scene
of objects, and the runtime that draws it. The renderer is `launcher/main/render/`;
the offline tools are `launcher/tools/r3d/`. There is no GPU and no display
framework, only a span rasterizer on both cores.

To put a model on screen, start with [Building a Scene](Building-a-Scene.md).

| | |
|---|---|
| [Building-a-Scene.md](Building-a-Scene.md) | **Start here**: import a mesh, place it in a scene, light it, add a camera, draw it. |
| [Scene-Files.md](Scene-Files.md) | The scene file: objects with a transform and one component (mesh renderer, directional light, camera), sky and ambient, what a scene must carry, and the scene entry (`SCNE`) the pack build bakes from it. |
| [Scene-Manager.md](Scene-Manager.md) | The runtime side of a scene: load by name, the packed component arrays, the one active camera, what the shell does each frame and who owns what. |
| [Mesh-Import.md](Mesh-Import.md) | The import file and the bake stages: opt-in processing steps, simplify, light, meshlets, and the baked format a renderer consumes. |
| [Mesh-Rendering.md](Mesh-Rendering.md) | The runtime: cameras, culling, the span rasterizer, instances at transforms, the two-core frame and the view modes. |

Related: [Animation Tracks](../Animation-Tracks.md) for the camera path,
[Firmware Architecture](../Firmware-Architecture.md) for the layers, and the
[render harness](../tools/Render-Harness.md) to see a scene without a board.
