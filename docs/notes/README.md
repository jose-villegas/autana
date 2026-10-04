# Platform Notes

Current board constraints and development rules for the Waveshare
ESP32-S3-Touch-AMOLED-1.8. Configuration and source references identify the
owners of settings; device tools provide measurements for the running image.

| Reference | Scope |
|---|---|
| [Board-and-Memory.md](Board-and-Memory.md) | hardware inventory, memory placement, caches, static and heap budgets, storage |
| [Display-and-Rendering.md](Display-and-Rendering.md) | panel initialization, DMA lifetime, clocks, dirty sends, screenshots and tearing |
| [Input-and-Sensors.md](Input-and-Sensors.md) | touch reports and calibration, sensor axes, PMU and GPIO buttons, tilt filtering |
| [Flashing-and-Toolchain.md](Flashing-and-Toolchain.md) | recovery, warm-reset handling, build configuration and compiler decisions |
| [Debugging.md](Debugging.md) | choosing tests, logs, captures, overlays and device measurements |

## Related

- [Firmware-Architecture.md](../Firmware-Architecture.md): shell and input
  ownership.
- [Testing-Guide.md](../Testing-Guide.md): host and device verification.
- [Build-Variants.md](../Build-Variants.md): instrumentation by build variant.
