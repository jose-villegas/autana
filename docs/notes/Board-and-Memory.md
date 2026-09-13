# Board and Memory

Part of the platform notes for the Waveshare ESP32-S3-Touch-AMOLED-1.8 - see
[`README.md`](README.md) for the full set. Everything here was verified on the
actual board or read out of the actual source - nothing is copied from a spec
sheet unless it is marked as such. Numbers come from boot logs and
`esp_timer` measurements taken in this repo.

Living document: correct it when the hardware disagrees with it.

---

## The board

Reported by the BSP at boot rather than assumed:

| Property | Value |
|---|---|
| Chip | ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz, single-precision FPU |
| Variant | auto-detected: **original** (SH8601 + FT3168) or **V2** (CO5300 + CST820) |
| Display | 368 × 448, QSPI, RGB565 |
| Touch / peripheral I2C | port 0, SDA GPIO 15, SCL GPIO 14 |
| Flash | 16 MB |
| PSRAM | 8 MB octal, 80 MHz |
| CPU | dual-core, `SOC_CPU_CORES_NUM 2` |

### Hardware inventory

Everything on the board, and how to tell it is alive. The POST that ships in
the firmware probes each of these at boot and prints exactly this table — see
`launcher/main/boot/post.c`.

| Peripheral | Part | Where | Notes |
|---|---|---|---|
| Display | SH8601 (original) / CO5300 (V2) | QSPI on SPI2 | 368×448 RGB565, 40 MHz |
| Touch | FT3168 (original) / CST820 (V2) | I2C `0x38` / `0x15` | which address answers identifies the board revision |
| IO expander | TCA9554 | I2C `0x20` | optional on this board; pulses the display/touch reset lines when fitted, skipped rather than faulted when absent |
| Power management | AXP2101 | I2C `0x34` | battery charging; the PWR button goes through it |
| IMU | QMI8658 | I2C `0x6B` | accelerometer + gyroscope; driver in `launcher/main/input/imu.c`, axes in [Input-and-Sensors.md](Input-and-Sensors.md) |
| Real-time clock | PCF85063 | I2C `0x51` | |
| Audio codec | ES8311 | I2C `0x18` | speaker + mic; amp enabled via a direct GPIO (46), not an IO-expander pin |
| microSD | — | native SDMMC, 1-bit, its own dedicated pins | fully independent of the display bus — see [SD card](#sd-card--fully-independent-of-the-display) |
| Flash | — | SPI0/1 | 16 MB, memory-mapped, never contended |
| PSRAM | — | SPI0/1, octal | 8 MB — the framebuffer lives here, see [Memory](#memory--the-constraint-that-shapes-everything) |
| Wi-Fi 4 / BLE 5 | on-die | — | radios present |
| BOOT button | — | GPIO 0, pull-up | active low, bounces; also the flashing button |
| PWR button | via AXP2101 | I2C `0x34` | not wired to the SoC at all — see [Input-and-Sensors.md](Input-and-Sensors.md) |
| Temperature sensor | on-die | — | |

All the I2C parts share one bus (port 0, SDA 15, SCL 14), so a single probe per
address establishes whether each is addressable. That is the cheapest possible
health check and what the POST is built on.

The two peripherals worth calling out are genuinely tested rather than assumed:

- **SD card** — mounted and unmounted as a real POST probe before
  `gfx_init()` brings the display up. Since the two now sit on entirely
  separate buses (see below), the ordering is bookkeeping, not a bus-contention
  requirement. An absent card is reported as optional rather than failing the
  board.
- **Audio codec** — the ES8311 sits behind a dedicated GPIO amplifier-enable
  line (not an IO-expander pin, since the expander itself is optional on this
  board), so POST raises that GPIO before probing (and lowers it again).
  Probing without it reports a working codec as missing. Note the datasheet
  address 0x30 is 8-bit; I2C wants the 7-bit `0x18`.

The board has two hardware revisions, both supported by this one firmware
build. `board_detect()` auto-detects which is fitted by probing the touch
controller's I2C address — CST820 at `0x15` means **V2** (CO5300 panel),
FT3168 at `0x38` means **original** (SH8601 panel). Always go through
`board_detect()` rather than hardcoding a driver.

Note the CO5300 (V2) needs an X-offset of `0x10` that the SH8601 (original)
does not. The panel bring-up applies it via `esp_lcd_panel_set_gap()`, so code
that drives the panel directly should not add its own.

---

## Memory — the constraint that shapes everything

8 MB of octal PSRAM (80 MHz, the fastest mode this die supports) changes what
"the constraint" even means here compared to a board without it. The
framebuffer (`BOARD_FRAMEBUFFER_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` in
`board.h`, 64-byte aligned to the cache line) is allocated entirely in PSRAM
and read in place by the panel's SPI DMA via the panel IO config's
`psram_dma_direct` flag — there is no internal-DRAM bounce copy, and internal
DRAM headroom is no longer shared with the framebuffer the way it would be on
a PSRAM-less board.

| Measurement | Value | Source |
|---|---|---|
| Internal (non-PSRAM) free heap after `gfx_init()` | **311,775 bytes** | `launcher/tools/device_profiles/esp32s3.sh`'s `DP_FREE_HEAP_BYTES`, device capture on the diagnostics build, 2026-09-13 |

That figure already excludes the framebuffer, since the framebuffer no longer
competes for it — it is the headroom left for everything else (task stacks,
app state, the sand grid, test fixtures in non-release builds).

### What fits

| Buffer | Size | Verdict |
|---|---|---|
| Full-screen RGB565 framebuffer | 322 KiB | lives in PSRAM, not counted against the internal heap above |
| One 64-row RGB565 strip | 46 KiB | trivial |
| Falling-sand grid (184×224) | 41,216 bytes | the largest single contiguous allocation of interest; unchanged by the port |

`small3dlib` (the cube renderer) still owns no framebuffer of its own — it
hands each pixel to a callback — and with `S3L_Z_BUFFER 0` keeps no depth
buffer, resolving visibility by sorting triangles back-to-front instead of
allocating a depth buffer. That choice no longer needs justifying on a memory
basis the way it would on a PSRAM-less board, but the code is unchanged and
the sorted-visibility caveat still holds: it is not pixel-exact — it cannot
resolve intersecting geometry — but for convex solids it is correct.

### Static growth still taxes the internal heap

Internal DRAM is one unified pool behind `.text`, `.bss`, `.data` *and* the
non-PSRAM heap — a new file-scope `static` array anywhere in `main/` (not
just the file you're editing) permanently reserves that space for the whole
process lifetime, competing with every other internal-heap allocation. This
has caused on-device OOM incidents before, from unrelated files, in unrelated
build variants — see [Optimization-Playbook.md](Optimization-Playbook.md)'s
"Test and debug code shares your production memory budget" for the full
story and the checklist for avoiding a repeat: an app's own big buffers get
malloc'd once and kept; anything optional (screenshots, debug overlays,
future dev tooling) must be malloc'd-on-use and freed-after, never a
permanent static, and must be checked with `idf.py -B build.dev size` /
`build.diag size` — not just `build.release`, which does not even compile
that code in.

There is no automated build-time gate for this any more: the framebuffer no
longer lives in internal DRAM, so the old prediction of "does the framebuffer
plus one grid still fit" no longer applies, and nothing has replaced it.
Watching internal-heap headroom (the measured figure above, and `HEAPMARK`
boot lines on a dev build) is a manual habit now, not an enforced one.

Reading `esp_get_free_heap_size()` against
`heap_caps_get_largest_free_block()` still invents a fragmentation gap that
is not there if the two are read from different pools: `esp_get_free_heap_size()`
sums a second, physically separate ~11 KiB DMA region (the ROM-stack area)
that is never contiguous with the main heap, so comparing it against
`heap_caps_get_largest_free_block()`'s single-region answer manufactures a
gap that was never real (see `check_memory()` in `launcher/main/boot/post.c`,
and [Optimization-Playbook.md](Optimization-Playbook.md) for the general
lesson). Free and largest must be read from the *same*
`heap_caps_*(MALLOC_CAP_DMA)` pool before comparing them.

### Task stacks are not the heap

FreeRTOS gives each task its own small fixed stack, a few KiB. A large local
variable silently overruns it. This cost us a boot loop:

```
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
Detected in task "main" at 0x4200b2f8
--- app_main at <file>:23
```

Line 23 was the *opening brace* of `app_main` — a rasterizer context of roughly
5 KiB had been declared as a stack local. Anything that size must be
`malloc`'d. Note the panic points at the function's entry, not at any statement
inside it, which is the signature of blowing the stack on frame setup.

Also worth knowing: the AMOLED retains its last frame in its own GRAM, so a
crash-looping app shows a stale image rather than going black. A frozen picture
is not evidence the firmware is alive.

---

## Storage

### Flash — usable during rendering

Flash sits on its own bus (SPI0/1) and does not contend with the display. It is
memory-mapped via `esp_partition_mmap()`, so it reads like a normal array with
no I/O calls.

Current partition use is `nvs` + `phy_init` + 3 MB app ≈ 3.1 MB of 16 MB,
leaving **~12.8 MB** for a data partition. For scale, a 256×256 RGB565 texture
is 128 KB — about 100 of them at zero RAM cost.

Caveat: mapped reads go through the CPU cache. Sequential access is fast,
random access thrashes. For texture sampling that argues for a tiled/swizzled
layout rather than row-major — the same reason GPUs store textures tiled.

### SD card — fully independent of the display

The microSD slot is native 1-bit SDMMC (`esp_driver_sdmmc`) on its own
dedicated pins, entirely separate from SPI2 and the QSPI lines the display
uses. This is different from a board where the display and an SD card share
one SPI bus: there is no bus to hand back and forth, no teardown/rebuild
dance, and no frozen-frame window while the card is in use. `bsp_sdcard_mount()`
and `bsp_sdcard_unmount()` can be called at any time regardless of whether the
display is up.

POST mounts the card once at boot (before `gfx_init()`, as a matter of
ordering convenience, not necessity) and can remount it live to catch a card
inserted or removed later — both are plain, independent SDMMC operations; see
`launcher/main/boot/post.c`.

### SD capacity limits

ESP-IDF's bundled FatFs has `FF_FS_EXFAT 0` — exFAT is compiled out.

- **≤ 32 GB** — ships FAT32, works as-is
- **> 32 GB** — ships exFAT, **will not mount**; reformat to FAT32 and it works
  (FAT32 + 32-bit LBA covers up to 2 TB)

Two more gotchas: `CONFIG_FATFS_LFN_NONE=y` means **8.3 filenames only**
(`TEXTURE.BIN` fine, `cube_texture_hi.bin` not), and the slot runs at
`SDMMC_FREQ_DEFAULT` = 20 MHz over native 1-bit SDMMC, so expect ~1–2 MB/s.

---

## Related

- [Display-and-Rendering.md](Display-and-Rendering.md) — panel bring-up and
  the rest of the SPI2 story, on top of these constraints.
- [Flashing-and-Toolchain.md](Flashing-and-Toolchain.md) — the toolchain and
  build-flag notes that affect the numbers referenced above.
