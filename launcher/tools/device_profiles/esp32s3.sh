#!/bin/sh
#
# Device profile: Waveshare ESP32-S3-Touch-AMOLED-1.8 (the board this repo
# is written for). Read the format's own rules in ../device_profile.sh
# before editing: plain KEY=value, no logic, no command substitution - both
# POSIX sh and device_profile.py parse this file.
#
# Every number here carries its provenance in the *_SOURCE field beside it.
# A number without a source is a guess, and a guess in this file silently
# becomes a gate somewhere else.

DP_NAME=esp32s3
DP_DESC="Waveshare ESP32-S3-Touch-AMOLED-1.8, octal PSRAM, 368x448 AMOLED"
DP_STATUS=measured

# --- main task stack -------------------------------------------------------
# What a test fixture's locals actually live inside on device. The host's
# stack is megabytes, which is why a 24 KB fixture array passed on a laptop
# and panic-looped the board twice.
DP_MAIN_TASK_STACK_BYTES=3584
DP_MAIN_TASK_STACK_SOURCE="CONFIG_ESP_MAIN_TASK_STACK_SIZE in launcher/sdkconfig, read 2026-09-13"

# Per-function stack-frame ceiling the host checker enforces on test code.
# Justified in launcher/test/check_stack_usage.py's header - short version:
# both historical panics (24 KB and 4 KB frames) are caught with two orders
# of magnitude of margin, while the largest legitimate fixture frame in the
# tree today is far below it.
DP_TEST_FRAME_CEILING_BYTES=1024
DP_TEST_FRAME_CEILING_SOURCE="derived from DP_MAIN_TASK_STACK_BYTES; see check_stack_usage.py"

# --- heap ------------------------------------------------------------------
# Internal heap free after gfx_init() (HEAPMARK). The framebuffer lives in
# PSRAM on this board (see board.h's BOARD_FRAMEBUFFER_CAPS), so it is not
# subtracted here - PSRAM is not counted: hot allocations (sand grids and
# the like) are meant to stay internal.
DP_FREE_HEAP_BYTES=184171
DP_FREE_HEAP_SOURCE="internal heap free after gfx_init (HEAPMARK, S3 diag build, 2026-09-14). PSRAM is not counted: hot allocations are meant to stay internal"

# One sand grid, for scale: a single contiguous request this size is why
# fragmentation - not just total bytes - decides whether a fixture runs.
DP_LARGEST_ALLOC_BYTES=41216
DP_LARGEST_ALLOC_SOURCE="one sand grid (184x224), unchanged by the port"

# Total PSRAM capacity - not a post-boot free figure the way DP_FREE_HEAP_BYTES
# is. No post-init free-PSRAM measurement is recorded anywhere in this file, so
# a test that wants realistic PSRAM headroom charges its own buffers (the
# framebuffer included) against this pool via heap_caps_malloc, same as device.
DP_PSRAM_BYTES=8388608
DP_PSRAM_SOURCE="ESP32-S3R8 package, 8 MB octal (launcher/sdkconfig.defaults CONFIG_SPIRAM_MODE_OCT); docs/notes/Board-and-Memory.md board table, PSRAM row"

# Below this many bytes, an allocation that names neither MALLOC_CAP_INTERNAL
# nor MALLOC_CAP_SPIRAM tries internal RAM before PSRAM; at or above it, PSRAM
# is tried first. Read directly from the sdkconfig knob, not derived.
DP_SPIRAM_ALWAYSINTERNAL_BYTES=65536
DP_SPIRAM_ALWAYSINTERNAL_SOURCE="CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL in launcher/sdkconfig and sdkconfig.defaults, read 2026-09-16"

# One SPI transaction's hard ceiling: the peripheral's own transaction-length
# register is 18 bits wide (SPI_LL_DMA_MAX_BIT_LEN in ESP-IDF's spi_ll.h), so
# one spi_transaction_t cannot carry more than this many bytes no matter how
# many DMA descriptors chain it. Not one of this file's usual three sources
# (no sdkconfig knob names a SoC register width) - recorded with the fullest
# provenance available: docs/notes/Display-and-Rendering.md's 80 MHz
# investigation lists "sub-windows under 32 KiB" among the transaction shapes
# it tried. Not wired to a build gate yet.
DP_DMA_MAX_TRANSACTION_BYTES=32768
DP_DMA_MAX_TRANSACTION_SOURCE="ESP-IDF spi_ll.h SPI_LL_DMA_MAX_BIT_LEN (18-bit transaction-length register); corroborated by docs/notes/Display-and-Rendering.md's 80 MHz investigation"

# --- toolchain and codegen -------------------------------------------------
DP_TOOLCHAIN_PREFIX=xtensa-esp32s3-elf
DP_TOOLCHAIN_SOURCE="ESP-IDF v5.5 tools/tools.json, xtensa-esp-elf esp-14.2.0_20260121"

# ISA-targeting flags. These are for a cross build ONLY and must never be
# copied onto a host compile - that is the whole distinction this file draws.
DP_ARCH_FLAGS="-mlongcalls -fno-builtin-memcpy -fno-builtin-memset -fno-builtin-bzero -mdisable-hardware-atomics"
DP_ARCH_FLAGS_SOURCE="launcher/build.dev/toolchain/cflags, read from a real S3 build, 2026-09-13"

# Codegen-SHAPING flags: they change what code the compiler emits for
# portable C, not which instruction set it emits it in, so a host harness
# that wants to predict device cost must use them too.
#
# -fno-jump-tables / -fno-tree-switch-conversion come from ESP-IDF's own
# top-level CMakeLists.txt (v5.5, line 273) and are target-independent, so
# every switch is a compare chain on every IDF target. Attempt 19's
# dispatcher regression was partly this.
DP_CODEGEN_FLAGS="-O2 -fno-jump-tables -fno-tree-switch-conversion -fstrict-volatile-bitfields -ffunction-sections -fdata-sections"
DP_STD_FLAG="-std=gnu17"
DP_CODEGEN_SOURCE="launcher/build.dev/compile_commands.json, sand_reactions.c entry, read from a real S3 build, 2026-09-13"

# --- instruction cache -----------------------------------------------------
# The S3's icache is Kconfig-selectable (CONFIG_ESP32S3_INSTRUCTION_CACHE_*);
# these are this project's sdkconfig choice, not a fixed property of the
# chip. Data cache is 32 KB, same line and way count, but this project has
# no field for it yet.
DP_ICACHE_BYTES=32768
DP_ICACHE_LINE_BYTES=32
DP_ICACHE_WAYS=8
DP_ICACHE_SOURCE="launcher/sdkconfig.defaults sets CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB; _LINE_32B and _8WAYS (data cache: _DATA_CACHE_32KB, _LINE_32B, _8WAYS) are IDF defaults read from the generated launcher/sdkconfig of a real S3 build, 2026-09-13"

# --- QEMU route ------------------------------------------------------------
# Espressif's QEMU fork models the S3 directly, so the primary route here is
# running the real diag image under `idf.py qemu`, not a generic-virt
# portable-sim route.
DP_QEMU_ROUTE=espressif-machine
DP_QEMU_SYSTEM_BIN=qemu-system-xtensa
DP_QEMU_MACHINE=esp32s3
DP_QEMU_CPU=
DP_QEMU_ESPRESSIF_TARGET=esp32s3
DP_QEMU_SOURCE="IDF v5.5 tools/tools.json: qemu-xtensa supported_targets ['esp32','esp32s3'], inspected 2026-09-03"
