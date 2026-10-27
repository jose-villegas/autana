# Internal memory opportunities

Status: investigation. Each row is a separate decision; none is applied.

The [generated scratch table](../memory/Internal-Memory-Map.md#what-could-move-out-of-iram)
and [variant map](../memory/Internal-Memory-Map.md#placement-rules-and-gate) hold the measurements.

Flash reads, NVS commits, writes and erases can disable caches. An ISR
registered without ESP_INTR_FLAG_IRAM waits for cache restoration; an
IRAM-safe ISR and all its callees and data must stay accessible. The panel
SPI bus and touch GPIO service use interrupt flags zero. Keeping a driver
ISR in IRAM does not itself register that ISR as cache-off safe.
<sup>[[59]](../Citations.md#espidf-spi-master)</sup><sup>[[63]](../Citations.md#espidf-flash-concurrency)</sup>

## Ranked opportunities

Ordered by increasing validation scope: product diagnostics/features, caller-audited
placements, timing-sensitive runtime paths, then memory and compilation policy.
Within a group the order is a review sequence, not a measured risk score.

| Opportunity / scratch row | Recommendation and risk | Maintenance cost | Board validation | Verified source |
|---|---|---|---|---|
| silent-assertions | Candidate for release: assertions and checks lose diagnostic text. The assertion remains enabled; see the scratch row for freed bytes. | Kconfig choice; retain useful diagnostics in development. | Boot and deliberate failure capture on a disposable test image; confirm PC symbolization remains useful. | Kconfig COMPILER_OPTIMIZATION_ASSERTIONS_SILENT and COMPILER_OPTIMIZATION_CHECKS_SILENT; size guide <sup>[[61]](../Citations.md#espidf-binary-size)</sup> |
| dynamic-log-level | Candidate only if runtime log-level changes are not required; the scratch choice also selects no per-tag level list. Coordinate with the [log-level plan](Log-Level-Plan.md). | Kconfig choice and product-level logging decision. | Console and logging smoke tests; confirm expected compile-time filtering. | log/Kconfig.level_settings LOG_DYNAMIC_LEVEL_CONTROL and LOG_TAG_LEVEL_IMPL_NONE; size guide <sup>[[61]](../Citations.md#espidf-binary-size)</sup> |
| encrypted-read-write | Candidate for builds without flash encryption. Disables encrypted read/write APIs; encryption support is a product decision. | Track any future encrypted-storage requirement. | Boot and NVS read/write; confirm encryption remains disabled. | [spi_flash Kconfig, SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE](https://github.com/espressif/esp-idf/blob/v5.5/components/spi_flash/Kconfig) |
| unused-flash-chips | Candidate after checking the fitted flash: removing auto-detect drivers can affect a replacement board revision. | Keep the board's chip selection and revision assumptions current. | Boot chip identification and NVS write/readback on each supported revision. | spi_flash/Kconfig SPI_FLASH_SUPPORT_*_CHIP; RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| ringbuf-to-flash | Candidate: console UART and USB-JTAG ISRs use flags zero. Audit any new IRAM-safe console caller. | Kconfig settings; preserve the ISR compatibility constraint. | Console traffic soak during NVS commits; measure interrupt response and dropped bytes. | esp_ringbuf/Kconfig; RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| libc-locks | Candidate after auditing cache-off callers. Libc locking operations gain flash cache misses. | Kconfig setting; preserve the caller audit as features change. | Concurrent logging, console traffic and NVS stress; allocation contention measurements. | esp_libc/Kconfig LIBC_LOCKS_PLACE_IN_IRAM; RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| gpspi-flash-hal | Candidate: this is flash-chip support on general-purpose SPI, separate from the panel SPI master path. Audit external flash users before moving it. | Symbol-level fragment selection must survive IDF symbol changes. | Panel soak and main-flash NVS read/write; check no external-flash API is registered. | spi_flash/linker.lf spi_flash_hal_gpspi mapping; flash concurrency <sup>[[63]](../Citations.md#espidf-flash-concurrency)</sup> |
| noflash-text | Needs caller audit: code remains in IRAM while its strings and constants can move into flash. An IRAM attribute alone does not make a caller cache-off safe. | A linker scheme change plus a data-access audit. | Present counters and console error paths during NVS stress; ensure no cache-off constant access. | esp_system/app.lf noflash and noflash_text schemes; cache-off constraints <sup>[[63]](../Citations.md#espidf-flash-concurrency)</sup> |
| freertos-non-isr | Needs measurement: `CONFIG_FREERTOS_IN_IRAM=n`, IDF's own split of FreeRTOS between flash and IRAM, retaining the queue calls used by presentation. `SPI_MASTER_IN_IRAM` depends on it, so the SPI queue leaves IRAM too. Delay and notification operations gain flash cache misses. | The kept queue calls are a scratch fragment, not a shipping fragment. | Interleaved present-counter and frame-budget A/B, including cold cache and NVS commits. | freertos/linker.lf, `FREERTOS_IN_IRAM`; SPI master guide <sup>[[59]](../Citations.md#espidf-spi-master)</sup> |
| heap-to-flash | Needs measurement: the scratch configuration disables both SPI master IRAM options. Its result includes that dependency change, not heap alone. A shipping experiment would need a reviewed SPI remap for the present path. | Maintain SPI queue/HAL selections and audit cache-off allocation callers, including development heap hooks. | Allocation/free microbench; present counters; frame-budget A/B; NVS stress. | heap/Kconfig; esp_driver_spi/Kconfig; RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| i2c-isr | Needs board measurement: moving the handler affects touch and IMU interrupt latency. Handler placement and I2C_ISR_IRAM_SAFE are separate settings. | Kconfig setting; audit ISR safety independently. | Input Lab latency distribution while presenting and committing NVS; IMU traffic soak. | esp_driver_i2c/Kconfig I2C_MASTER_ISR_HANDLER_IN_IRAM; cache-off constraints <sup>[[63]](../Citations.md#espidf-flash-concurrency)</sup> |
| RTC clock/time fragment selections | Needs a cache-off caller audit before a scratch fragment is safe; forced IRAM attributes cannot be moved by a fragment. | Symbol-level list tied to clock and PSRAM-retuning implementation. | Restart, panic and PSRAM-temperature soak; clock-change validation. | esp_hw_support/linker.lf and port/esp32s3/rtc_clk.c; cache-off constraints <sup>[[63]](../Citations.md#espidf-flash-concurrency)</sup> |
| Cold statics in RTC SLOW | Needs measurement: capacity and access speed differ from ordinary SRAM; not suitable for DMA. No scratch byte result is claimed. | Explicit storage ownership and reset/persistence policy. | CPU access microbench, reset correctness, POST heap and largest-block measurements. | ESP32-S3 memory layout and linked rtc_slow_seg; RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| Component-specific size optimization | Needs scoped performance A/B: scheduler, interrupt and SPI code may become slower. No scratch byte result is claimed. | CMake compilation policy maintained per IDF component. | Full frame-budget and present-counter A/B, console soak and NVS stress. | Build System, Controlling Component Compilation <sup>[[65]](../Citations.md#espidf-build-system)</sup> |
| XiP from PSRAM | Separate experiment: changes boot copy, PSRAM footprint and cache behavior. Asset mmap reads during erases need validation. No scratch byte result is claimed. | Configuration, boot and memory policy change; PSRAM retuning audit. | Boot timing, frame-budget A/B, concurrent asset reads and NVS writes, temperature soak. | External RAM and flash concurrency <sup>[[64]](../Citations.md#espidf-external-ram)</sup><sup>[[63]](../Citations.md#espidf-flash-concurrency)</sup> |

## Not worth it without a different requirement

| Item | Reason | Source |
|---|---|---|
| Forced XMC-C flash auto-suspend and dependent IRAM removals | The force-enable Kconfig help warns against enabling it; suspend timing and frequent LCD interrupts require a different flash-policy investigation. | spi_flash/Kconfig SPI_FLASH_FORCE_ENABLE_XMC_C_SUSPEND; optional flash features <sup>[[62]](../Citations.md#espidf-flash-optional-features)</sup> |
| SPI_FLASH_ROM_IMPL | Trades maintained flash fixes and chip support for ROM behavior; high-performance flash mode needs validation. | RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| Excluding already unlinked peripheral objects | Unreferenced objects do not occupy linked IRAM; use the generated map before proposing a component removal. | Build system link behavior <sup>[[65]](../Citations.md#espidf-build-system)</sup> |
| Reclaiming startup-stack and RTC FAST memory | The heap memory layout and startup release already own these regions; avoid a second allocator or overlapping region. | heap/port/esp32s3/memory_layout.c and freertos/app_startup.c; RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| Disabling memory protection for split slack | Loses memory protection for the generated alignment slack; inspect the map instead of claiming meaningful capacity. | esp_system/ld/esp32s3/sections.ld.in and soc/esp32s3/include/soc/soc_caps.h |
| Cache resizing without performance measurements | Cache capacity trades SRAM against instruction/data misses. The shipping configuration needs board evidence for a change. | RAM guide <sup>[[60]](../Citations.md#espidf-ram-usage)</sup> |
| Patching shared IDF linker templates | Project fragments can override archive placement at finer granularity; a shared install patch adds version and CI maintenance. | tools/ldgen/ldgen/generation.py; build system <sup>[[65]](../Citations.md#espidf-build-system)</sup> |
| ROM heap or systimer implementations unsupported by this target | Target capability constraints decide availability; do not assume an option from another chip applies. | heap and hal Kconfig capability dependencies |

## Measurement policy

Approve each opportunity independently. Start with the isolated scratch
result, inspect its changed components and effective sdkconfig, then measure
the resulting image on the board. Gate its placement budgets before flashing.
Compare runtime internal free heap and largest block as well as frame and
interrupt costs. A byte reduction without latency or reliability evidence is
an opportunity, not a shipping recommendation.
