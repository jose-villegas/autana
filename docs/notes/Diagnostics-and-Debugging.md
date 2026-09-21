# Diagnostics and Debugging

Part of the platform notes for the Waveshare ESP32-S3-Touch-AMOLED-1.8 - see
[`README.md`](README.md) for the full set.

What to reach for depends on what is actually wrong. This is organised by
symptom, not by tool - skim the table, jump to the matching section.

---

## Which tool, for what

| Symptom | Reach for |
|---|---|
| Board is unresponsive / will not flash | [Board won't boot](#board-wont-boot-or-wont-flash) |
| Logic might be wrong in code you're writing | [Host test suite](#is-the-logic-right---host-test-suite) - sub-second loop |
| Passes on host, not sure it holds on the real chip | [On-device test suite](#does-it-still-hold-on-the-real-chip---on-device-suite) |
| Need to see exactly what's on screen right now | [Screenshot + device state](#what-does-the-screen-look-like-right-now---autana-screenshot) |
| Need live logs, or a crash to resolve to file:line | [autana monitor](#live-logs-and-crash-backtraces---autana-monitor) |
| Need to send the board a command | [Sending the board a line](#sending-the-board-a-line) |
| A render looks wrong - stale pixels, wrong region sent | [gfx debug overlays](#rendering-looks-wrong---gfx-debug-overlays) |
| Stray pixels/lines on the glass that a screenshot does not show | [Panel-link faults](Display-and-Rendering.md#panel-link-faults-are-invisible-to-screenshots) |
| Frame rate / performance seems off | [Performance](#performance-seems-off) |
| Orientation or the IMU seems wrong | [Orientation and IMU](#orientation-or-the-imu-seems-wrong) |
| Suspected memory pressure | [Memory](#suspected-memory-pressure) |

---

## Board won't boot, or won't flash

Not a diagnosis question so much as a recovery one - see
[`Flashing-and-Toolchain.md`](Flashing-and-Toolchain.md) for the BOOT-button
recovery sequence and why auto-reset stops working once firmware goes idle.

Every build, release included, also runs POST at boot (`main/boot/post.c`):
I2C peripheral probes, flash size, heap headroom, MAC validity, the on-die
temperature sensor, the SD card. Silent when everything passes; on a
**failure** it holds the report on screen for 8 seconds or until touched, so
a board with a genuinely faulty component says so even with nobody attached
to a serial console. The full report is always available on demand from the
Diagnostics app (`--dev`/`--diag` builds only - see the next sections) if you
want to see it without waiting for a failure.

## Is the logic right? - host test suite

```bash
./launcher/test/run_tests.sh
```

Under a second, runs on this machine (not the chip), and covers every
*portable* suite - anything with no hardware dependency. This is the loop for
red-green-refactor; reach for it first for anything that is a question about
logic rather than about the actual board. See
[`../Testing-Guide.md`](../Testing-Guide.md).

## Does it still hold on the real chip? - on-device suite

```bash
autana selftest                                   # build, flash, run every suite
./launcher/tools/report_test_results.sh           # same, plus a markdown report
```

Builds the diagnostics variant, flashes it, and runs *every* registered
suite - portable ones included - actually compiled by the Xtensa toolchain
and executed on the chip, which a host run cannot vouch for. Needs a
`CONFIG_LAUNCHER_SELFTEST` build; see
[`../Build-Variants.md`](../Build-Variants.md) for what that flag carries
versus `--dev`.

## What does the screen look like right now? - `autana screenshot`

```bash
autana screenshot
```

Captures whatever is currently on screen as a lossless `.png`, plus a
same-named `.json` snapshot of device state at that exact frame - uptime,
heap (current and low-water mark), CPU clock, on-die temperature,
orientation, the IMU, and that frame's touch/button state. Good for anything
where you need to see the actual pixels, or correlate a visual glitch
against memory/sensor conditions at that instant - see
`main/console/console_screenshot.c` and `main/util/device_state.h` for the
mechanism and the full field list.

- The device streams a 24bpp BMP over the wire, decoded to PNG in memory
  (stdlib `zlib`/`struct`, no Pillow) before anything touches disk - the
  `.bmp` is never written.

- **Development-only** (`--dev` or `--diag` build) - a release build carries
  none of it.
- **Slow by design**: a full 368x448 frame is roughly 650 KB of base64 over
  115200 baud, taking the better part of a minute. `autana screenshot`
  prints progress every few seconds so this does not read as a hang.
- **Does not reset the board** - opens the port with DTR/RTS held low so a
  capture shows whatever app was already running, not a restarted boot
  animation.
- Takes the device lock, so it queues behind whatever else already holds
  the board rather than fighting it for the port.
- **Every render mode.** An indexed-colour app (256 or 16 colours) keeps no
  framebuffer; its frame is rebuilt row by row through the same expansion
  the present path sends. An app drawing in RGB565 bands keeps no image at
  all, so the capture forces one full redraw and copies each band into a
  temporary PSRAM snapshot as it is sent. If that cannot happen (the
  snapshot does not fit, or the app stops drawing), the device answers `SCREENSHOT_REFUSED:`
  with the reason and the command exits at once.
- **Blind to the panel link.** It captures the framebuffer, and the shell
  requests a full redraw right after, which heals a corrupted panel. Stray
  pixels or lines seen on the glass but not in the capture are a link fault;
  see "Panel-link faults are invisible to screenshots" in
  [`Display-and-Rendering.md`](Display-and-Rendering.md).

## Live logs and crash backtraces - `autana monitor`

```bash
autana monitor
```

Streams the console for a while (60 seconds when no argument is given), and
decodes any crash address it sees against an ELF's symbols - the build
directory whose own `build_id.txt` matches the capture's `BUILD_ID`, or
`--elf path/to/other.elf` to pin a specific one. Passing the right `.elf`
matters for more than bookkeeping - it carries the debug symbols that turn
a crash address into a file and line number. See
[`../tools/Autana-CLI.md`](../tools/Autana-CLI.md).

## The console is USB-Serial-JTAG, not UART0

**This board's single USB-C port is the ESP32-S3's own native USB-Serial/JTAG
peripheral**, not an external USB-UART bridge chip. UART0 exists on this
board too, but only broken out on separate solder pads - nothing a USB cable
ever reaches.

ESP-IDF's own default for a chip with this peripheral assumes the OTHER
common board design instead: UART0 as the primary console (read AND
written), USB-Serial-JTAG as a write-only secondary mirror (see
`esp_system/Kconfig`'s own `ESP_CONSOLE_SECONDARY` help text, which
describes this exact mismatch and names the fix). Left at that default,
logging over the one cable this board actually has looks completely normal -
every line shows up as expected - while anything sent the OTHER direction (a
typed idf_monitor command, `autana screenshot`'s trigger, anything) goes
nowhere: console reads only ever come from the primary channel, and
USB-Serial-JTAG was only ever the secondary.

**Fixed in `sdkconfig.defaults`**: `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y`
makes USB-Serial-JTAG the primary channel, matching what this board is
actually wired to. If idf_monitor ever prints

    Writing to serial is timing out. Please make sure that your application
    supports an interactive console and that you have picked the correct
    console for serial communication.

this is the first thing to check - `grep CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
sdkconfig` should show `=y`. A build directory generated before this was
fixed has the wrong choice baked into its own `sdkconfig`; delete the
directory and rebuild rather than expecting `sdkconfig.defaults` alone to
retroactively fix one that already exists.

## Sending the board a line

`autana monitor` only reads. To send, start a session with `autana` and
type a verb there (`help` lists them); each line takes the device lock,
sends, and lets go.

## Rendering looks wrong - gfx debug overlays

`--dev`/`--diag` builds carry two runtime overlays, toggled from the
Diagnostics app's developer-toggle page (`gfx_set_debug_overlay()` /
`gfx_set_leaf_overlay()` in `main/gfx/gfx.h`):

- **Dirty-region overlay** - draws a border around whatever rectangle
  `gfx_present()` is about to send, so a stale patch of screen (something
  drawn but never marked dirty) or an over-wide send (marked dirty when it
  should not have been) is visible directly rather than inferred from
  symptoms.
- **Leaf overlay** - outlines, in green, the leaves actually marked dirty
  this frame. Independent of the dirty-region overlay.

Both are off by default even in a development build, since they draw
directly over real content. A border shows for the one present that sent
it: the next present resends that strip clean, so what is on screen is
always the latest present's sends, not an accumulation.

## Performance seems off

The shell logs frames-per-second on a fixed timer (`report_fps()` in
`main/main.c`) - unconditionally, in every build including release, not
gated behind `CONFIG_LAUNCHER_DEVELOPMENT` the way other instrumentation is
(worth knowing if you go looking for it and expect it gated the same way as
everything else on this page - see the note in
[`../Build-Variants.md`](../Build-Variants.md#development-only-instrumentation-is-its-own-flag-not-selftest)
on what should be gated and why). `autana monitor` shows it directly, no special
build needed.

For anything deeper than an fps number: `app_sand.c` carries its own
`CONFIG_LAUNCHER_DEVELOPMENT`-gated rolling averages (step/draw timing,
awake-cell counts) logged periodically - see
`main/apps/sand/tools/report_performance.sh` for the host-side report
generator. The render lab app's cube scene has a dedicated on-device
performance suite (`main/apps/render_lab/suite_cube_perf.c`) for
phase-by-phase timing (logic / rasterise / HUD / present) against a 60fps
budget, run the same way as any other on-device suite (see
[above](#does-it-still-hold-on-the-real-chip---on-device-suite));
`main/apps/render_lab/tools/report_cube_perf.sh` is its host-side report.

## Orientation or the IMU seems wrong

Two ways to see raw sensor readings without adding any code:

- **Diagnostics app's "show orientation" toggle** (`--dev`/`--diag` build) -
  shows the raw accelerometer counts, the derived gx/gy display orientation
  is actually computed from, and the shell's current quarter-turn, all at
  once, so a physical hold can be pinned to an exact number.
- **An `autana screenshot` capture's `.json`** - the `imu` object (raw
  accelerometer + gyroscope counts) and `orientation_quarter` field are a
  snapshot at one specific frame, useful when the question is "what was the
  board reading at the moment this visual bug happened" rather than a live
  reading.

## Suspected memory pressure

- **POST's boot-time check** - fails outright (not just a warning) when the
  largest free DMA-capable block falls below `MIN_LARGEST_DMA_BLOCK` in
  `main/boot/post.c`, and reports that block plus free DMA heap on every
  boot, release included. Both figures come from `MALLOC_CAP_DMA`; reading
  either against `esp_get_free_heap_size()` compares different pools and
  invents a fragmentation gap that is not there (see
  [Board-and-Memory.md](Board-and-Memory.md)).
- **A dev build's `HEAPMARK` boot lines** - free and largest-contiguous DMA
  at each boot phase, plus one heap block map where the framebuffer lands.
  This is the fastest way to tell a static-footprint problem from an
  allocation-order one, and it is what settled that question in one boot.
- **An `autana screenshot` capture's `.json`** - `heap_free_bytes` (current) and
  `heap_min_free_bytes` (the low-water mark since boot - shows a transient
  allocation that already freed again, which `heap_free_bytes` alone
  cannot).

---

## Related

- [`../Build-Variants.md`](../Build-Variants.md) - what
  `CONFIG_LAUNCHER_DEVELOPMENT` and `CONFIG_LAUNCHER_SELFTEST` actually
  gate, and the three build variants (release/dev/diag).
- [`../Testing-Guide.md`](../Testing-Guide.md) - the host and device test
  runners, and runsuite.
- [`../Launcher-Architecture.md`](../Launcher-Architecture.md) - the
  Diagnostics app (DEVELOPMENT-gated as a whole, with the self-test runner
  alone narrowed to SELFTEST), and its still-open split into a Settings app.
- [`../plans/Settings-App-Plan.md`](../plans/Settings-App-Plan.md) - that open split,
  and the SELFTEST/"diagnostics" naming mismatch it would resolve.
- [`Flashing-and-Toolchain.md`](Flashing-and-Toolchain.md) - board recovery,
  and the toolchain details `autana monitor`'s crash decoding depends on.
