# Launcher tools

Host tooling for building, generating, rendering, inspecting, and checking the firmware.

| Folder | Purpose |
|---|---|
| [build/](build/README.md) | ESP-IDF build wrappers and host tool discovery. |
| [gen/](gen/README.md) | Generators for checked-in C data. |
| [r3d/](r3d/README.md) | Offline mesh baking for the r3d renderer: simplification, baked light, cluster trees. |
| [gltf/](gltf/) | Shared glTF 2.0 reader, sampler and writer. |
| [anim/](anim/README.md) | Bakes a glTF animation into C tracks and samples baked tracks on a host. |
| [render/](render/README.md) | Host render harness, image comparison, and [scenes](render/scenes/README.md). |
| [device/](device/README.md) | Board profiles, report capture, and screenshot decoding. |
| [quality/](quality/README.md) | Complexity, MISRA, and test report checks. |
| [asset/](asset/README.md) | Writes the asset pack container the firmware maps. |
| [boot_anim/](boot_anim/README.md) | Boot animation performance reports. |
| [sweeps/](sweeps/) | Build and capture sweeps. |
| [tests/](tests/) | Python regression tests for these tools. |
