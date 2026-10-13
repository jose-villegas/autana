# Board and Memory

Part of the [platform notes](README.md). Hardware selection lives in
`launcher/main/board/`; memory policy lives in `launcher/main/core/memory.h`
and `launcher/sdkconfig.defaults`.

## The board

ESP32-S3R8, dual-core Xtensa LX7 @ 240 MHz, single-precision FPU. Flash and
PSRAM clocks are set in `launcher/sdkconfig.defaults`.

```mermaid
flowchart LR
    SOC["ESP32-S3"]
    SOC --- QSPI["SPI2, QSPI"] --- PANEL["SH8601 / CO5300 panel<br/>368 x 448 RGB565"]
    SOC --- MEM["SPI0/1"] --- FL["flash 16 MB"]
    MEM --- PS["PSRAM 8 MB octal"]
    SOC --- SD["SDMMC, 1-bit"] --- CARD["microSD"]
    SOC --- G46["GPIO 46"] --- AMP["speaker amp"]
    SOC --- G0["GPIO 0"] --- BOOT["BOOT button"]
    SOC --- I2C["I2C0<br/>SDA 15, SCL 14"] --- DEV["touch 0x38 / 0x15<br/>AXP2101 0x34, PWR button<br/>QMI8658 0x6B<br/>PCF85063 0x51<br/>ES8311 0x18<br/>TCA9554 0x20, optional"]
```

The display, SD card and external memory use separate buses. CPU work and
internal-memory traffic can still contend; separate buses do not guarantee
independent throughput.

| Peripheral | Part | POST | Notes |
|---|---|---|---|
| Display | SH8601 (V1) / CO5300 (V2) | ✓ | 368×448 RGB565 |
| Touch | FT3168 (V1) / CST820 (V2) | ✓ | the address that answers identifies the revision |
| IO expander | TCA9554 | optional | resets display and touch when fitted |
| Power management | AXP2101 | ✓ | battery charging, the PWR button ([Input-and-Sensors.md](Input-and-Sensors.md)) |
| IMU | QMI8658 | ✓ | axes in [Input-and-Sensors.md](Input-and-Sensors.md) |
| Real-time clock | PCF85063 | ✓ | |
| Audio codec | ES8311 | ✓ | datasheet address `0x30` is 8-bit; I2C wants `0x18` |
| microSD | - | optional | see [SD card](#sd-card) |
| Flash | - | ✓ | memory-mapped |
| PSRAM | - | ✓ | 8 MB octal |
| Wi-Fi 4 / BLE 5 | on-die | via `soc` | reported, not exercised |
| BOOT button | - | | active low, bounces; also the flashing button |
| Temperature sensor | on-die | ✓ | |

POST (`launcher/main/selftest/post.c`) also checks the MAC/eFuse and the I2C bus
itself. It raises the amp GPIO before probing the codec, which does not answer
without it (`board_audio_amp_enable()`).

### Revisions

| Revision | Touch answers at | Panel | X gap |
|---|---|---|---|
| V2 | CST820 `0x15` | CO5300 | `0x10` (`BOARD_PANEL_X_GAP`) |
| original (V1) | FT3168 `0x38` | SH8601 | none |

`board_detect()` tells them apart; go through it rather than hardcoding a
driver, and do not add the gap yourself.

## Memory: the constraint that shapes everything

| Tier | Current placement | Rule |
|---|---|---|
| Internal SRAM | stacks, ordinary mutable statics, DMA gather and bounce buffers, hot working data | budget static storage and the largest allocation together |
| PSRAM | full framebuffer (`MEMORY_PSRAM`) and app arena | keep frequently accessed working data internal where it fits |
| Flash | executable code, ordinary `static const` tables and mapped assets | account for cache misses on bulk or scattered reads |

In full-framebuffer mode the framebuffer is in PSRAM; band and indexed modes
free it. The band ring aliases the internal DMA bounce slots. See
[Gfx-and-Presentation.md](../Gfx-and-Presentation.md) for presentation.
The app arena (`launcher/main/app/app_arena.h`) uses explicit external-memory
placement, so its reservation reduces the PSRAM heap before apps allocate.

`CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=65536` makes ordinary allocations up to
that threshold **try** internal RAM first, with PSRAM fallback. This is a
preference, not a placement guarantee. `memory_alloc()` requests the named
kind explicitly and does not fall back to another kind. Use `MEMORY_INTERNAL`
for required internal placement and `MEMORY_DMA` for the panel's buffers.

Read current free-heap and largest-block measurements from POST and the
development build's `HEAPMARK` lines. The host model's budget is recorded in
`launcher/tools/device/device_profiles/esp32s3.sh`; it is not a live heap
measurement of every build variant.

### PSRAM throughput

Device measurements put PSRAM reads at 33–58 MB/s depending on access
pattern, versus 365–724 MB/s for internal RAM. Sequential copies measure
about 58 MB/s out of PSRAM, 47 MB/s into it and 22 MB/s from PSRAM to PSRAM.
Writes incur cache-line loads before writeback, so bulk copies are costly
beside the arithmetic they support. These are measured access-pattern
ranges, not a guarantee for a particular buffer or clock configuration.
Prefer internal SRAM for hot working data where it fits, and read PSRAM
for bulk or cold content. The [presentation memory policy](../Gfx-and-Presentation.md#presentation-memory-policy)
explains why presentation keeps one retained framebuffer.

### Cycle budgets

At 240 MHz, one core has about 24 cycles per physical pixel at 60 fps,
48 at 30 fps, or 97 per half-resolution pixel at 60 fps. These are clock
and pixel-count budgets, not measured rendering costs.

### Cache is carved from the same pool

| Cache | Build configuration | Access |
|---|---|---|
| Instruction | 32 KiB, 32-byte lines, 8-way | flash-resident instructions |
| Data | 64 KiB, 64-byte lines, 8-way | mapped flash data and PSRAM |

`launcher/sdkconfig.defaults` selects the instruction-cache size with
`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` and the data-cache size and line size
with `CONFIG_ESP32S3_DATA_CACHE_64KB` and `CONFIG_ESP32S3_DATA_CACHE_LINE_64B`;
the remaining settings use ESP-IDF's ESP32-S3 cache defaults. The larger data
cache serves the renderer's PSRAM vertex, depth and colour streams. Cache
storage consumes internal SRAM: increasing its capacity reduces the internal
heap available to allocations. Internal working data avoids external-memory
cache traffic;
flash-resident `const` data is not internal working data. Inspect the linker
map for actual placement, including explicitly RAM-mapped code and tables.
Changing code layout can change cache behaviour even when a hot function's
instructions are identical; measure the linked image on the device.

### Internal memory map

IRAM and DRAM are aliases of the same shared SRAM pool. Code placement
reduces the space available for static data and the internal heap.
`launcher/tools/build/memory_map.py` reads each build's own map, ELF,
sdkconfig and linked scripts. The measured block is refreshed by CI;
its SHA-256 marker lets host gates detect hand edits without building firmware.

An IRAM site's immediately preceding comment gives a reason and `IRAM budget: N B`.
A function definition is a site when it carries an IRAM attribute (including an alias)
or its immediately preceding comment declares an IRAM budget; placement may be
inherited from a declaration. IRAM-placing linker-fragment mappings are sites too.
When sizes differ, use `IRAM budget: release R B, dev D B, diag T B`
with measured integers in place of R, D and T. Each build selects its variant
from `build_id.txt`. On the IDF commit in
[`launcher/idf-pin.json`](../../launcher/idf-pin.json), budgets match in both
directions. Other IDF commits report budget drift as a note naming both commits. The reason, after removing the budget, must contain at least
two words and sentence-ending punctuation; TODO, TBD and FIXME stubs fail.
Placement options in `launcher/sdkconfig.defaults*` need an adjacent reason,
including `*_PLACE_*_INTO_FLASH=n`, but no budget for IDF-owned code.
Engine and app sites are reported separately. Ownership comes from the build's
component directories under this repository, excluding managed components.
Without a build, discovery follows
[`launcher/CMakeLists.txt`](../../launcher/CMakeLists.txt) and IDF's project
component discovery rule.

| Gate finds | Result |
|---|---|
| Our linked IRAM symbol has no site | Fail |
| Site lacks an adjacent reason or required budget | Fail |
| Measured bytes differ from the variant's budget, larger or smaller | Fail on pinned IDF; drift note otherwise |
| Per-variant budget omits this build's variant | Fail |
| Enabled site places nothing | Fail |
| Compiler or linker-fragment configuration excludes a site | Note |

Run `python launcher/tools/build/memory_map.py --source-only` to check comments
without a build. Run `python launcher/tools/build/memory_map.py --check
launcher/build launcher/build.dev launcher/build.diag` to check linked bytes.

`--update-budgets` with those build directories rewrites only each active site's
`IRAM budget:` clause, preserving its reason and line endings. It requires every
variant in `memory_map.VARIANTS` on the pinned IDF commit and refuses a partial
or unpinned build set. Equal active sizes use a plain budget; differing sizes
use per-variant budgets. Compiled-out sites contribute no active budget.

<!-- generated: internal-memory-map sha256=a557563dbe5169270afcbc9a6109ec90e149aff373ff71d71d57f2086c0d07ec -->
> Generated by: `python launcher/tools/build/memory_map.py launcher/build launcher/build.dev launcher/build.diag --write`

Bytes from each build's ELF, link map, sdkconfig and ESP-IDF size JSON. Heap is a link-time range, not runtime free memory.

| Region / boundary | e530a95ff240-release (IDF 9a97f6c54ec6) | 9203fd2622aa-dev (IDF 9a97f6c54ec6) | 5be46fd813ee-diag (IDF 9a97f6c54ec6) |
|---|---|---|---|
| iram0_0_seg shared alias | 0x40378000–0x403cb700 (341760 B) | 0x40378000–0x403cb700 (341760 B) | 0x40378000–0x403cb700 (341760 B) |
| dram0_0_seg shared alias | 0x3fc88000–0x3fcdb700 (341760 B) | 0x3fc88000–0x3fcdb700 (341760 B) | 0x3fc88000–0x3fcdb700 (341760 B) |
| DIRAM used (idf_size) | 145210 | 166282 | 181502 |
| .dram0.bss | 45808 | 56288 | 70192 |
| .dram0.data | 16763 | 20011 | 20575 |
| .dram0.dummy | 82688 | 90112 | 90880 |
| .dram0.heap_start | 0 | 0 | 0 |
| .iram0.bss | 0 | 0 | 0 |
| .iram0.data | 0 | 0 | 0 |
| .iram0.text | 81611 | 88955 | 89707 |
| .iram0.text_end | 49 | 129 | 145 |
| .iram0.vectors | 1028 | 1028 | 1028 |
| Split granularity | 256 | 256 | 256 |
| Prefetch pad | 16 | 16 | 16 |
| Slack including pad | 49 | 129 | 145 |
| _iram_end | 0x4038c300 | 0x4038e000 | 0x4038e300 |
| _data_start | 0x3fc9c300 | 0x3fc9e000 | 0x3fc9e300 |
| _bss_start | 0x3fca0480 | 0x3fca2e30 | 0x3fca3360 |
| _bss_end | 0x3fcab770 | 0x3fcb0a10 | 0x3fcb4590 |
| _heap_start | 0x3fcab770 | 0x3fcb0a10 | 0x3fcb4590 |
| _heap_low_start | 0x3fcab770 | 0x3fcb0a10 | 0x3fcb4590 |

Split follows `CONFIG_ESP_SYSTEM_MEMPROT_FEATURE`; alignment and prefetch padding are read from the linked scripts and map.

| IRAM component | Ownership | release | dev | diag |
|---|---|---|---|---|
| libfreertos.a | IDF / managed | 13727 | 14020 | 14793 |
| libesp_hw_support.a | IDF / managed | 13882 | 13874 | 13874 |
| libspi_flash.a | IDF / managed | 12522 | 12576 | 12576 |
| libheap.a | IDF / managed | 6151 | 6206 | 6206 |
| libesp_driver_spi.a | IDF / managed | 5143 | 5143 | 5143 |
| libesp_hal_mspi.a | IDF / managed | 5093 | 5093 | 5093 |
| libmain.a | ours | 887 | 4750 | 4750 |
| libesp_system.a | IDF / managed | 3592 | 3592 | 3562 |
| libesp_hal_gpspi.a | IDF / managed | 3082 | 3082 | 3082 |
| libesp_libc.a | IDF / managed | 2432 | 2722 | 2722 |
| libesp_ringbuf.a | IDF / managed | 0 | 2598 | 2598 |
| libxtensa.a | IDF / managed | 2496 | 2512 | 2512 |
| libesp_psram.a | IDF / managed | 1920 | 1920 | 1920 |
| libhal.a | IDF / managed | 1873 | 1873 | 1873 |
| libesp_lcd.a | IDF / managed | 1713 | 1713 | 1713 |
| libesp_mm.a | IDF / managed | 1261 | 1261 | 1261 |
| libbootloader_support.a | IDF / managed | 1018 | 1018 | 1018 |
| libesp_driver_i2c.a | IDF / managed | 819 | 819 | 819 |
| libesp_rom.a | IDF / managed | 711 | 711 | 711 |
| libesp_hal_systimer.a | IDF / managed | 555 | 555 | 555 |
| libesp_hal_dma.a | IDF / managed | 517 | 517 | 517 |
| libxt_hal.a | IDF / managed | 405 | 405 | 405 |
| liblog.a | IDF / managed | 354 | 383 | 383 |
| libesp_driver_gpio.a | IDF / managed | 354 | 354 | 354 |
| libwaveshare__esp_lcd_sh8601.a | IDF / managed | 327 | 327 | 327 |
| libesp_driver_dma.a | IDF / managed | 129 | 129 | 129 |
| libesp_driver_usb_serial_jtag.a | IDF / managed | 114 | 114 | 114 |
| libgcc.a | IDF / managed | 0 | 104 | 104 |
| libesp_timer.a | IDF / managed | 65 | 65 | 65 |
| libefuse.a | IDF / managed | 10 | 10 | 10 |
| Linker fill / split slack | unattributed | 1536 | 1666 | 1691 |

| Owner | Placement site | Symbols | release | dev | diag | Budget B | Reason |
|---|---|---|---|---|---|---|---|
| engine | launcher/main/gfx/present/gfx_present.c:127 | on_strip_sent | 36 | 36 | 36 | 36 | The panel bus uses interrupt flags 0, so this callback need not survive cache-off (docs/Citations.md#59); IRAM only spares it cache misses. |
| engine | launcher/main/input/touch.c:71 | on_touch_int | 50 | 50 | 50 | 50 | Need not be cache-off safe: esp_lcd_touch installs the GPIO ISR service with flags 0. Kept in IRAM until a touch-latency measurement says flash is fine. |
| engine | launcher/main/linker.lf:22 | gather_and_send, gfx_present, gfx_present_begin, gfx_present_wait, present_task_fn, run_present_normal, send_fb_rows | 801 | 3993 | 3993 | release 801, dev 3993, diag 3993 | A present's own code in IRAM: from flash, a narrow strip went from ~236 us to ~398 us with the cache cold (suite_gfx test_narrow_present_counters). Setup, other present modes and the SPI driver's own queue keep their configured placement. CONFIG_LAUNCHER_DEVELOPMENT adds overlays, send auditing and raw-full dispatch, so dev and diag are larger. |
| engine | launcher/main/linker.lf:35 | esp_lcd_panel_draw_bitmap, esp_lcd_panel_io_tx_color, esp_lcd_panel_io_tx_param, lcd_spi_post_trans_color_cb, panel_io_spi_tx_color, panel_io_spi_tx_param | 1574 | 1574 | 1574 | 1574 | Avoid cache misses in the LCD dispatch and SPI completion path used by every present. |
| engine | launcher/main/linker.lf:47 | panel_sh8601_draw_bitmap | 327 | 327 | 327 | 327 | Avoid cache misses in the panel draw dispatch used by every present. |
| engine | launcher/main/profile/frame_watch.c:111 | heap_call_site | 0 | 419 | 419 | 419 | Heap hooks and their callees must remain callable while flash cache is disabled. |
| engine | launcher/main/profile/frame_watch.c:127 | esp_heap_trace_alloc_hook | 0 | 22 | 22 | 22 | Heap hooks and their callees must remain callable while flash cache is disabled; IDF's declaration in esp_heap_caps.h places this one in IRAM. |
| engine | launcher/main/profile/frame_watch.c:139 | esp_heap_trace_free_hook | 0 | 22 | 22 | 22 | Heap hooks and their callees must remain callable while flash cache is disabled; IDF's declaration in esp_heap_caps.h places this one in IRAM. |
| engine | launcher/main/profile/frame_watch.c:61 | watching_this_task | 0 | 78 | 78 | 78 | Heap hooks and their callees must remain callable while flash cache is disabled. |
| engine | launcher/main/profile/frame_watch.c:77 | record | 0 | 130 | 130 | 130 | Heap hooks and their callees must remain callable while flash cache is disabled. |

#### Differs between variants

Exclusive symbols are absent from IRAM in every other listed variant. The console reports the full symbol-level diff.

| Component | release | dev | diag | release only | dev only | diag only |
|---|---|---|---|---|---|---|
| libfreertos.a | 13727 | 14020 | 14793 |  |  | tasks.c.obj:vTaskSuspend (313 B), tasks.c.obj:xTaskGenericNotifyWait (226 B), tasks.c.obj:prvTaskCreateDynamicPinnedToCoreWithCaps (193 B), tasks.c.obj:uxTaskGetStackHighWaterMark (58 B), tasks.c.obj:uxTaskGetNumberOfTasks (13 B) |
| libesp_hw_support.a | 13882 | 13874 | 13874 |  |  |  |
| libspi_flash.a | 12522 | 12576 | 12576 |  |  |  |
| libheap.a | 6151 | 6206 | 6206 |  |  |  |
| libmain.a | 887 | 4750 | 4750 |  |  |  |
| libesp_system.a | 3592 | 3592 | 3562 |  |  |  |
| libesp_libc.a | 2432 | 2722 | 2722 |  |  |  |
| libesp_ringbuf.a | 0 | 2598 | 2598 |  |  |  |
| libxtensa.a | 2496 | 2512 | 2512 |  |  |  |
| liblog.a | 354 | 383 | 383 |  |  |  |
| libgcc.a | 0 | 104 | 104 |  |  |  |

#### Release shared block

The split slack includes the prefetch pad. Boxes show linked bytes in address order; width is schematic.

```mermaid
flowchart LR
    iram["IRAM<br/>82639 B"] --- slack["Split slack<br/>49 B"] --- data["DRAM data<br/>16763 B"] --- bss["BSS<br/>45808 B"] --- heap["Heap range<br/>196496 B"]
```
<!-- /generated: internal-memory-map -->

### What could move out of IRAM

<!-- generated: internal-memory-what-if sha256=17dd2a39ef334b5fccf23682a804875582090c6daad16df62444937c7daf27be -->
> Generated by: `python launcher/tools/build/iram_what_if.py launcher/build launcher/build.dev launcher/build.diag --work launcher/build/iram-what-if --jobs 6 --write`

Bytes are scratch-build deltas on the local IDF named below; latency and cache-off safety are not board-measured. Positive IRAM/DRAM deltas are bytes freed. Heap-start delta is negative when the heap grows. Each row is isolated; savings are not additive.

0325f2830a0e-release (IDF c197d718bcc2); fef8605a0a93-dev (IDF c197d718bcc2); e40d13d8131c-diag (IDF c197d718bcc2)

| Variant | Experiment | IRAM freed B | DRAM freed B | Heap-start delta B | Change |
|---|---|---|---|---|---|
| release | ringbuf-to-flash | 160 | 0 | -256 | CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y, CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH=y |
| release | silent-assertions | 6484 | 5224 | -11880 | CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE=n, CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_SILENT=y, CONFIG_COMPILER_OPTIMIZATION_CHECKS_SILENT=y |
| release | unused-flash-chips | 3820 | 1388 | -5224 | CONFIG_SPI_FLASH_SUPPORT_ISSI_CHIP=n, CONFIG_SPI_FLASH_SUPPORT_MXIC_CHIP=n, CONFIG_SPI_FLASH_SUPPORT_GD_CHIP=n, CONFIG_SPI_FLASH_SUPPORT_WINBOND_CHIP=n, CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP=n, CONFIG_SPI_FLASH_SUPPORT_TH_CHIP=n, CONFIG_SPI_FLASH_SUPPORT_MXIC_OPI_CHIP=n |
| release | encrypted-read-write | 596 | 188 | -952 | CONFIG_SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE=n |
| release | libc-locks | 948 | 0 | -1024 | CONFIG_LIBC_LOCKS_PLACE_IN_IRAM=n |
| release | i2c-isr | 728 | 0 | -768 | CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=n |
| release | dynamic-log-level | 992 | 264 | -1288 | CONFIG_LOG_TAG_LEVEL_IMPL_NONE=y, CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=n |
| release | heap-to-flash | 15352 | 952 | -16312 | CONFIG_SPI_MASTER_IN_IRAM=n, CONFIG_SPI_MASTER_ISR_IN_IRAM=n, CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y |
| release | noflash-text | 0 | 668 | -664 | our `(noflash)` entries become `(noflash_text)` |
| release | gpspi-flash-hal | 1844 | 0 | -1792 | libhal `spi_flash_hal_gpspi*` to flash |
| release | freertos-non-isr | 6884 | 0 | -6912 | IDF's flash-placement selections, except FromISR, port and present-path queue calls |
| dev | ringbuf-to-flash | 4132 | 0 | -4096 | CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y, CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH=y |
| dev | dynamic-log-level | 992 | 280 | -1304 | CONFIG_LOG_TAG_LEVEL_IMPL_NONE=y, CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=n |
| dev | heap-to-flash | 15404 | 952 | -16312 | CONFIG_SPI_MASTER_IN_IRAM=n, CONFIG_SPI_MASTER_ISR_IN_IRAM=n, CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y |
| dev | noflash-text | 0 | 668 | -664 | our `(noflash)` entries become `(noflash_text)` |
| dev | freertos-non-isr | 6920 | 0 | -6912 | IDF's flash-placement selections, except FromISR, port and present-path queue calls |
| diag | ringbuf-to-flash | 4132 | 0 | -4096 | CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y, CONFIG_RINGBUF_PLACE_ISR_FUNCTIONS_INTO_FLASH=y |
| diag | dynamic-log-level | 992 | 280 | -1304 | CONFIG_LOG_TAG_LEVEL_IMPL_NONE=y, CONFIG_LOG_DYNAMIC_LEVEL_CONTROL=n |
| diag | heap-to-flash | 15404 | 952 | -16312 | CONFIG_SPI_MASTER_IN_IRAM=n, CONFIG_SPI_MASTER_ISR_IN_IRAM=n, CONFIG_HEAP_PLACE_FUNCTION_INTO_FLASH=y |
| diag | noflash-text | 0 | 668 | -672 | our `(noflash)` entries become `(noflash_text)` |
| diag | freertos-non-isr | 7560 | 0 | -7680 | IDF's flash-placement selections, except FromISR, port and present-path queue calls |
<!-- /generated: internal-memory-what-if -->

Recommendations, costs, citations and board validation live in the
[internal memory opportunities plan](../plans/Internal-Memory-Opportunities.md).

### Static growth still taxes the internal heap

Ordinary mutable file-scope statics reserve internal DRAM for the whole boot,
including those in development and self-test code. Explicit PSRAM placement
and flash-resident constant tables have different costs. Allocate optional
debug buffers on use and mutable large test fixtures per test, then free them.
Clean up earlier allocations before an assertion can abort a failing fixture.

Check `.bss`, `.data` and RAM-resident code in the linker map and size report
for every build variant (release, `--dev`, `--diag`). From `launcher/`, run
`idf.py -B build.dev size` and `idf.py -B build.diag size` as well as the
release size report. Link-time size does not prove that a contiguous block
will remain available after runtime allocations.

Compare `memory_free_bytes(MEMORY_DMA)` and
`memory_largest_block(MEMORY_DMA)` from the same pool. Total free heap across
other capabilities does not establish whether a DMA buffer fits. Watch POST
and `HEAPMARK` after changing either static storage or allocation order.

### Task stacks are not the heap

```
Guru Meditation Error: Core 0 panic'ed (Stack protection fault).
Detected in task "main" at 0x4200b2f8
--- app_main at <file>:23
```

| You see | It means | Do |
|---|---|---|
| a stack protection fault pointing at a function's opening brace | a large local overran the task stack on frame setup | `malloc` it |
| a frozen picture after a crash | the panel's GRAM keeps the last frame | do not take it as the firmware running |

## Storage

### Flash

`launcher/partitions.csv` keeps 64 KB after the app partition for the last
panic's core dump ([Debugging](Debugging.md)) and gives the rest of the flash
to one data partition, `assets`, which holds the [asset pack](../assets/README.md).
`esp_partition_mmap()` reads it like an array; mapped reads go
through the cache, so sequential access is fast and random access thrashes.

### SD card

`bsp_sdcard_mount()` and `bsp_sdcard_unmount()` work at any time, display up
or not. POST mounts and releases the card before `gfx_init()` and can remount
it live.

| Filesystem | Mounts |
|---|---|
| FAT32 | yes |
| exFAT | no (`FF_FS_EXFAT 0`); reformat to FAT32 first |

8.3 filenames only (`CONFIG_FATFS_LFN_NONE`); 20 MHz (`SDMMC_FREQ_DEFAULT`),
1-bit.

## Related

- [Display-and-Rendering.md](Display-and-Rendering.md): panel constraints on SPI2.
- [Flashing-and-Toolchain.md](Flashing-and-Toolchain.md): build configuration
  and compiler checks.
