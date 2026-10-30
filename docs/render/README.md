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

| Start | |
|---|---|
| [Building-a-Scene.md](Building-a-Scene.md) | **Start here**: import a mesh, place it in a scene, light it, add a camera, draw it. |
| [Render-Pipeline.md](Render-Pipeline.md) | **The overview**: every stage from the source model to the panel in order, what each reads, writes and costs, with a picture and a link to its reference page. |
| [Rendering-Options.md](Rendering-Options.md) | **Choosing options**: every framebuffer layout, present policy and r3d knob that trades work for speed or look, what each costs and when to use it. |

| Folder | Pages |
|---|---|
| [offline/](offline/README.md): import, bake, fit, on the host | [Mesh-Import](offline/Mesh-Import.md), [Bake-Method](offline/Bake-Method.md), [Appearance-Fit](offline/Appearance-Fit.md), [Bake-Quality](offline/Bake-Quality.md), [Baked-Mesh](offline/Baked-Mesh.md) |
| [scene/](scene/README.md): the scene file and its runtime | [Scene-Files](scene/Scene-Files.md), [Scene-Manager](scene/Scene-Manager.md), [Camera](scene/Camera.md) |
| [animation/](animation/README.md): tracks and skinned meshes | [Skinned-Meshes](animation/Skinned-Meshes.md), [Animation-Tracks](animation/Animation-Tracks.md), [Skeleton-and-Skin](animation/Skeleton-and-Skin.md), [Skinned-Lighting](animation/Skinned-Lighting.md) |

| Every frame, on the board | |
|---|---|
| [Mesh-Rendering.md](Mesh-Rendering.md) | The runtime: culling, the span rasterizer, instances at transforms, the two-core frame and the view modes. |
| [Dynamic-Resolution.md](Dynamic-Resolution.md) | The render size picked per frame to hold a budget: where a frame's time goes at each size, the stepped controller and the predictor. |
| [Frame-Generation.md](Frame-Generation.md) | Frames warped from the last render to the next camera pose and shown between renders: quality, board cost, pricing into dynamic resolution, the calls an app makes. |

Related: [Firmware](../firmware/README.md) for the layers, and the
[render harness](../tools/Render-Harness.md) to see a scene without a board.
