# Platform Notes

Current board constraints and development rules for the Waveshare
ESP32-S3-Touch-AMOLED-1.8. Configuration and source references identify the
owners of settings; device tools provide measurements for the running image.

| Reference | Scope |
|---|---|
| [../memory/Internal-Memory-Map.md](../memory/Internal-Memory-Map.md) | internal pool, IRAM placement budgets, variant differences and isolated experiments |
| [../memory/Linker-Fragments.md](../memory/Linker-Fragments.md) | placing code or data with an ESP-IDF linker fragment, and when an attribute is better |
| [Board-and-Memory.md](Board-and-Memory.md) | hardware inventory, memory placement, caches, static and heap budgets, storage |
| [Display-and-Rendering.md](Display-and-Rendering.md) | panel initialization, DMA lifetime, clocks, dirty sends, screenshots and tearing |
| [../audio/README.md](../audio/README.md) | audio path, sound assets and captured loopback measurements; the board's audio wiring is in [Board-and-Memory.md](Board-and-Memory.md#audio) |
| [../power/README.md](../power/README.md) | datasheet-derived current budget per power state |
| [Input-and-Sensors.md](Input-and-Sensors.md) | touch controller, IMU and PMU chip facts; the input features are in [../input/README.md](../input/README.md) |
| [Flashing-and-Toolchain.md](Flashing-and-Toolchain.md) | recovery, warm-reset handling, build configuration and compiler decisions |
| [Debugging.md](Debugging.md) | choosing tests, logs, captures, overlays and device measurements |

## Related

- [firmware/README.md](../firmware/README.md): shell and input
  ownership.
- [Testing-Guide.md](../testing/README.md): host and device verification.
- [Build-Variants.md](../tools/Build-Variants.md): instrumentation by build variant.
