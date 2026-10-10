# Plans

Designs written before or during the work they describe. Each plan's own
**Status** line says whether it is planned, partly built or built.

| | |
|---|---|
| [Autana-Rendering-Roadmap.md](Autana-Rendering-Roadmap.md) | Future rendering work, automated regression gates and the three target games. |
| [UI-Editor-Plan.md](UI-Editor-Plan.md) | A UI editor and the authored, baked layout format underneath it. |
| [Settings-App-Plan.md](Settings-App-Plan.md) | Splitting Diagnostics into a Settings app, and unifying SELFTEST/diagnostics naming. |
| [Networking-Plan.md](Networking-Plan.md) | What the board's radios allow, their RAM cost, and updating the firmware over the air. |
| [Log-Level-Plan.md](Log-Level-Plan.md) | A compile-time log-level ceiling per build variant. |
| [Cluster-LOD.md](Cluster-LOD.md) | Levels of detail over the baked meshlets: the pick rule, what it saved, and the branch that holds the tooling. |
| [Qemu-Target-Plan.md](Qemu-Target-Plan.md) | Driving an emulated image from autana, and what it cannot answer. |
| [Motion-Design-Plan.md](Motion-Design-Plan.md) | Motion for the launcher: springs and easing, a sliding Control Center, a cached blurred backdrop, app open and close, orientation morphs. |
| [Reaction-Doc-Generator-Plan.md](Reaction-Doc-Generator-Plan.md) | A generated short description for every sand brush. |
| [Animation-Bindings-Design-Sketch.md](Animation-Bindings-Design-Sketch.md) | Animation clips as bindings (object path, component, field) resolved once at load, so a clip drives any animatable field. |
| [Content-in-the-Pack-Design-Sketch.md](Content-in-the-Pack-Design-Sketch.md) | Animation tracks and scenes as asset-pack entries baked from their source files, one pack per root asset, and poses from the device's own sampler. |
| [Cached-Bakes-Design-Sketch.md](Cached-Bakes-Design-Sketch.md) | Expensive bakes (lit meshes, GPU fits, Blender exports) as files named by a digest of their inputs, made by CI and fetched by every build instead of committed. |
| [Image-Kernels-Plan.md](Image-Kernels-Plan.md) | Real-time blur and edge detection over the framebuffer: packed-RGB565 tricks, box blurs, SIMD, and ranked first experiments, every cost an estimate. |
| [Animation-System-Design-Sketch.md](Animation-System-Design-Sketch.md) | Property and character animation: the whole shape, and milestone 1, a skinned model playing, blending and picking its clips. |
