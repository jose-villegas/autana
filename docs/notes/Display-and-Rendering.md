# Display and Rendering

Part of the [platform notes](README.md). The frame's send path is documented
in [Gfx-and-Presentation.md](../Gfx-and-Presentation.md); these are the panel
constraints that path must obey.

The panel refreshes from its own GRAM. A region that is not sent retains its
last pixels, including after firmware crashes.

## Bring-up

`launcher/main/gfx/gfx.c` owns SPI2, panel IO and panel initialization.
`board_detect()` selects SH8601 with FT3168 touch or CO5300 with CST820 touch.
Keep revision detection and panel offsets in the board layer rather than
hardcoding a driver or applying the gap in drawing code.

The panel-specific init tables in `gfx.c` are required alongside the driver;
driver defaults alone do not supply the board's panel settings.

| Constraint | Guard |
|---|---|
| Bitmap submission queues an asynchronous DMA read | do not rewrite a submitted buffer until its transfer completes |
| Multiple transfers can finish before the caller waits | `strip_sent` is a counting semaphore, with one take per queued transfer |
| The panel expects byte-swapped RGB565 | construct colours through `gfx_rgb()` |
| Panel windows require even edges | round windows outward with `even_floor()` and `even_ceil()` |
| External-memory buffers cannot supply the fast panel path reliably | send through internal `MEMORY_DMA` bounce slots |

## The panel link

### Transfer payload

A full frame sends the panel dimensions times two bytes over four QSPI lanes.
CPU optimisation cannot reduce that payload's bus time; dirty regions and
partial bands reduce bytes sent. Rendering and DMA can overlap, so timing an
isolated band does not predict the cost of a pipelined frame.

`CONFIG_LAUNCHER_GFX_QSPI_80MHZ` sets the boot clock, `GFX_QSPI_HZ`, in
`launcher/main/gfx/gfx.h`. The shell may change it at run time through
`gfx_set_panel_clock_hz()`. The SPI divider provides 40 or 80 MHz for these
choices; requesting an intermediate value does not provide an intermediate
panel clock.

### 80 MHz is outside the panel's rating

The 80 MHz option exceeds the panel's rated clock and can leave stray pixels
or lines during partial redraws. Even window edges are required at both
clocks; they do not remove the clock risk. Use 40 MHz for a clean link.
The shell owns clock selection and partial-redraw healing; see
[Panel clock and heal](../Gfx-and-Presentation.md#panel-clock-and-heal).

### Panel-link faults are invisible to screenshots

`autana screenshot` captures source pixels, not panel GRAM. The running frame
loop requests a full redraw after capture, which can heal corrupted panel
pixels; a frozen frame is not redrawn. Judge a suspected link fault on the
glass. The development-only `gfx_set_send_audit()` checks whether changed
pixels were sent with the correct bytes; a clean audit does not prove the
panel received them correctly.

## Transaction setup

A panel transaction has setup cost as well as payload cost. Sending one row
per call can cost more than a larger merged transfer. Gather and run-merging
thresholds `GATHER_MAX_PIXELS` and `LEAF_REFINE_MAX_RUNS` in
`launcher/main/gfx/gfx_dirty.h` are fitted for 40 MHz; remeasure on the
device before changing the clock or those thresholds.

## Dirty-region costs

The device tests in `launcher/test/suites/suite_gfx.c` time full bands,
narrow changes, short wide changes and separated marks against each other.
Use those tests for current costs rather than transferring isolated timings
between clocks or render modes. Geometry and marking ownership are in
[Dirty tracking](../Gfx-and-Presentation.md#dirty-tracking) and
`launcher/main/gfx/gfx_dirty.h`.

Full-width partial bands are contiguous and need no gather packing. Narrow
runs use bounce slots; the queued transfer must finish before its slot is
reused. Preserve that lifetime when changing the send path.

## Tearing and the TE line

Both panel init tables enable tearing-effect output with `0x35 0x00`.
The panel drives TE on FPC pin 2, GPIO13 (the schematic's LCD_TE net);
firmware does not configure that pin.
Firmware does not synchronize presentation to TE; a present starts when the
frame loop reaches it. A write can tear when it crosses the panel's scan.
Waiting for TE alone would not make a transfer tear-free: render and send
must remain on one side of the scan throughout the update, and the wait adds
latency. Measure scan timing and the complete send path before adding a wait.

## No graphics acceleration

The ESP32-S3 has no pixel-processing accelerator or GPU. Rendering is CPU
work. The shell runs the frame loop on core 0; the present task and job worker
use core 1. A parallel stage uses `launcher/main/util/job.h`; see
[Mesh-Rendering.md](../render/Mesh-Rendering.md#on-both-cores).

## Related

- [Board-and-Memory.md](Board-and-Memory.md): buses and buffer placement.
- [Debugging.md](Debugging.md): send audits, captures and device timing.
