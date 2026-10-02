# Platform Notes

Working notes for the Waveshare ESP32-S3-Touch-AMOLED-1.8. Everything here was
verified on the actual board or read out of the actual source: nothing is
copied from a spec sheet unless it is marked as such. Numbers come from boot
logs and `esp_timer` measurements taken in this repo.

Six files, one hardware fact area each; an app's own discovery narrative
lives with that app's documents instead:

- **[Board-and-Memory.md](Board-and-Memory.md)**: the board's hardware
  inventory, the memory budget built around the framebuffer living in
  PSRAM, and why the SD card and the display do not share a bus.
- **[Display-and-Rendering.md](Display-and-Rendering.md)**: the path of a
  frame to the panel, bring-up and its silent failures, the bus-bound
  blit and the QSPI clock (80 MHz is outside the panel's rating and corrupts
  partial redraws; no clock between 40 and 80 exists), why screenshots cannot
  see a panel-link fault, the fixed cost per call, the dirty-tracking
  measurements, the tearing line, and the rejected and parked ideas.
- **[Input-and-Sensors.md](Input-and-Sensors.md)**: touch, the IMU's axes
  and its accelerometer/gyroscope split, and the two buttons that are not
  the same kind of device.
- **[Flashing-and-Toolchain.md](Flashing-and-Toolchain.md)**: recovering
  an unresponsive board, ESP-IDF version requirements, and the build flag
  and frame-tick rules a framerate comparison has to respect.
- **[Optimization-Playbook.md](Optimization-Playbook.md)**: general-purpose
  performance techniques this board's work turned up, written to travel to
  other chips and projects rather than staying specific to this one:
  measuring by deleting code instead of reasoning about it, verifying
  `static inline` actually inlined with `objdump`, register-spilling call
  boundaries, and more.
- **[Debugging.md](Debugging.md)**: which
  tool to reach for depending on the symptom: the host and on-device test
  suites, `autana screenshot`'s image-plus-device-state capture,
  `autana monitor`, the gfx debug overlays, and the USB-Serial-JTAG console
  quirk that breaks typing into idf_monitor if you don't know to look for it.

## Related

- [`../Firmware-Architecture.md`](../Firmware-Architecture.md): how the
  shell and its apps are built on top of the hardware facts here.
- [`../Testing-Guide.md`](../Testing-Guide.md): how any of this gets
  verified on real hardware.
- [`../Build-Variants.md`](../Build-Variants.md): which image carries the
  instrumentation these pages tell you to read.
