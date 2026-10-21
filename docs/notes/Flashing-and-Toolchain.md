# Flashing and Toolchain

Part of the platform notes for the Waveshare ESP32-S3-Touch-AMOLED-1.8; see
[`README.md`](README.md) for the full set.

---

## Flashing and recovery

Keep `app_main` running: the shell's frame loop never exits, and startup
error paths park in a sleep loop. Firmware that returns and goes idle can
leave this board unresponsive to auto-reset, including esptool's RTS reset.
A successful flash does not establish that the application booted.

If the board becomes unreachable, BOOT has to be held at the moment power
arrives, so what produces that moment decides the procedure.

| # | No battery fitted | Battery fitted |
|---|---|---|
| 1. | Unplug USB-C | **Long-press PWR** (~10 s), USB-C still plugged in, until the COM port disappears |
| 2. | **Hold BOOT** | **Hold BOOT** |
| 3. | Plug USB-C back in, still holding | **Press PWR**, still holding |
| 4. | Keep holding ~2 s, release | Keep holding ~2 s, release |

**With a battery fitted, unplugging USB never power-cycles the board**, and
fails silently: the AXP2101 keeps the rail up from the battery, so the SoC
carries its stuck state through every replug. Only the PMU cuts a
battery-backed rail.

Either sequence forces the ROM bootloader regardless of firmware state.
From there `autana flash` writes the image and ends with esptool's RTS
reset. What a flash proves (esptool's hash check and the build's
`BUILD_ID`, not the boot) and the whole hand-off under the device lock are
in [Flash-and-Captures.md](../tools/Flash-and-Captures.md#what-a-flash-proves).

The RTS reset is a warm reset, so the bootloader's PMIC restart (below)
power-cycles the SoC: USB drops and comes back, possibly on a **different
COM number**. Nothing depends on the number: after every reset `device.py`
waits for the board's USB serial number to leave the port list and come
back, then opens the port it is listed under, so no open gets the dying
handle. Every open `device.py` makes leaves DTR and RTS low without passing
through the USB Serial/JTAG reset state (DTR low, RTS high); `open_serial`
in [device.py](../../scripts/device/device.py) says how on each OS. `selftest` and
`batch` check the console `BUILD_ID` against the flashed image.

If it vanishes from USB entirely (no COM port, no device at vendor ID
`0x303A`), check
the cable first, then the PWR button: this board's power is managed by an
**AXP2101 PMIC**, so a long press cuts system power.

### Warm resets at 120 MHz

At 120 MHz, ESP-IDF's PSRAM timing tuning sweeps the BBPLL, which also clocks
the USB PHY. A host that is talking to the USB Serial/JTAG port during the
sweep is left with a link that still takes RTS resets but carries no data:
the console goes silent and esptool cannot sync until the port is replugged
or power-cycled. After a warm reset the host is always on the port; after a
power cycle it depends on how soon the host enumerates the port again.
[board_usb.c](../../launcher/main/board/board_usb.c) wraps the tuning
(`--wrap=mspi_timing_psram_tuning` in `launcher/main/CMakeLists.txt`) and turns
the D+ pull-up off for it, so the host sees a short unplug instead. A debugger
attached over USB-JTAG loses the port for that unplug on every boot.
`launcher/tools/build/check_psram_tuning_wrap.py` fails a linked image whose
tuning is not reached through the wrap.

Independently of the wrap, the bootloader component
`launcher/bootloader_components/pmic_cold_boot/` requests an AXP2101 power
cycle whenever the reset was not a power-on.

```mermaid
sequenceDiagram
    participant Host
    participant Boot as 2nd-stage bootloader
    participant PMIC as AXP2101
    participant App
    Host->>Boot: warm reset
    Boot->>PMIC: I2C, REG 0x10 bit 1, restart
    PMIC->>Boot: VCC3V3 off and on, PWROK low on CHIP_PU
    Note over Host,Boot: USB drops, the port goes away
    Boot->>App: power-on reset this time, so the app loads
    Note over Host,App: D+ pull-up off while the app tunes PSRAM
    App->>Host: first BUILD_ID, before the port is back
    Note over Host,App: USB enumerates again
    App->>Host: BUILD_ID again, after shell Ready
```

The power cycle drops the USB port, and Windows discards serial data the host
had not read yet, so unread panic output can be lost. Development and
diagnostics builds set `CONFIG_PMIC_COLD_BOOT_HOST_DRAIN` in their config
fragments and wait for `HOST_DRAIN_US` before the cycle; release does not.
The bootloader configuration cannot see `CONFIG_LAUNCHER_DEVELOPMENT`. A
power-on reset never waits.

After a successful PMIC restart the app reports a power-on reset: after a panic
or a watchdog the cause shows only in what was logged before it, RTC memory
does not survive, and a deep-sleep wake would become a full power cycle.
Do not rely on RTC state surviving that restart. The bootloader logs and
continues if the PMIC does not acknowledge or the restart does not occur.

---

## Toolchain

- **ESP-IDF v6.1+ is required.** `launcher/main/idf_component.yml` declares
  `idf: ">=6.1"`; CI builds on the exact commit `launcher/idf-pin.json` names
  ([Framework-Versions.md](../tools/Framework-Versions.md)). Several versions
  can coexist; [Which ESP-IDF](../tools/Build-Environment.md#which-esp-idf)
  describes pin selection and one-command `IDF_PATH` overrides.
- On another ESP-IDF commit the IRAM budget check reports a mismatch as a
  drift note instead of failing, and `memory_map.py --update-budgets` refuses
  to run.
- On Windows, `launcher/tools/build/idf.sh` invokes `export.bat` through
  `cmd` with the MSYS environment removed. A working `idf.py` or PowerShell
  activation alone is insufficient: `export.bat` must have batch-compatible
  line endings and a Python environment it can locate. ESP-IDF's
  `install.bat` prepares that environment.
- **`IDF_TOOLS_PATH` is the root, not the `tools/` inside it.** Point it one
  level too deep and `idf_tools.py` installs a second copy of every toolchain
  under `tools/tools/`.
- BSP component: the vendored `launcher/components/esp32_s3_touch_amoled_1_8/`,
  plus the panel drivers main declares directly to see their headers:
  `espressif/esp_lcd_co5300` (V2) and `waveshare/esp_lcd_sh8601` (original).
- `sdkconfig.defaults` worth keeping: `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`;
  without it the image header says 2 MB and the bootloader warns on every boot.

`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` makes the native USB-Serial/JTAG
peripheral the primary console for both reads and writes. See
[Debugging.md](Debugging.md#the-console-is-usb-serial-jtag-not-uart0).

## The build flag and the frame tick

`CONFIG_COMPILER_OPTIMIZATION_PERF` selects -O2 in
`launcher/sdkconfig.defaults`. A generated `sdkconfig` is not automatically
re-derived when defaults change. `launcher/tools/build/idf_variant.sh`
invalidates configuration older than its fragments; use `autana build` for
the variant you need.

The frame loop in `launcher/main/shell/shell.c` calls `timing_yield()`, which uses `vTaskDelay(1)` in
`launcher/main/core/timing_device.c`. `CONFIG_FREERTOS_HZ` sets the tick.
Frame rate includes scheduler quantisation; compare device microseconds of
work when assessing small changes.

### Verify compiler decisions

`static inline` is a request. Inspect the Xtensa object's symbol table and
the caller's disassembly with the toolchain's objdump; a separate function
symbol alone does not tell whether every call site was inlined. Check for
remaining calls and register spills. `always_inline`, used by
`launcher/main/core/memory.h` and `launcher/main/core/timing.h`, also needs
verification at its call sites. Inlining grows callers and can increase
instruction-cache pressure, so time the final linked image.

The direct C sources in `launcher/main/render/` receive
`-falign-functions=${CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE}` from
`launcher/main/CMakeLists.txt`. Function placement within an instruction-cache
line stays fixed when unrelated code ahead of it grows or shrinks. Alignment
stabilizes timing; it does not select the fastest loop offsets. Explicit function
alignment attributes take precedence and their offsets are baseline-checked. The draw
path's entry functions carry `RENDER_IRAM_ATTR` from
`launcher/main/render/code_layout.h` and run from IRAM, where no cache line
applies. A hot flash function can carry `CODE_ENTRY_OFFSET` from the same header: it
starts the function on a cache line, whatever code links ahead of it, and
never-executed `nop.n` padding ahead of the entry places its loops at the
offsets measured fastest on the board. The release build workflow runs
`launcher/tools/render/code_layout.py --check` against
source-local `code_layout.txt` baselines for the selected build configuration.
Rows cover function entries and the small loops inside them; the checker
compares offsets, sizes and cache-line spans. When a row changes, compare revisions
with the performance suite that owns the function through `launcher/tools/perf/perf_compare.sh`,
then re-record the baseline with `--write --baseline`.
See [Pinned code layout](../../launcher/tools/render/README.md#pinned-code-layout).

Do not use `-falign-loops` for this on Xtensa. GCC aligns the label after a
zero-overhead `loop`, the assembler fills the gap with zeros, and the CPU
executes those bytes as an IllegalInstruction.

### Arithmetic in hot loops

The target is a 32-bit Xtensa core. A general signed 64-bit divide uses the
software helper `__divdi3`; constant divisors can be optimized differently.
Check widening inside fixed-point helpers, prove operand bounds before
narrowing, and inspect disassembly rather than assigning a source-level
divide a fixed cycle cost. `r3d_project_point_cs_x()` in
`launcher/main/render/r3d_project_x.h` keeps the per-point divide 32-bit
and widens only the focal-length multiply. Near-plane clipping in
`math/linear/projectx.h` uses a 64-bit divide for its Q16.16 crossing ratio.

`ceilf()` is a libm call, too costly per row; use `(int)x` plus one when it
falls short. Float division (`__divsf3`) is the FPU's exact
`div0.s`/`divn.s` sequence behind a call. Where a hot loop needs 1 / z,
`mathf_recip()` in `launcher/packages/math/include/math/scalar/mathf.h` is cheaper: the FPU's
`recip0.s` seed and two Newton steps, within an ulp of the quotient; its
comment holds the board's cycle counts.

Signed division rounds toward zero. An arithmetic right shift rounds negative
values differently, so signed division by a power of two can require rounding
instructions as well as a shift. Use unsigned arithmetic only where the
value's range and semantics permit it.

## Related

- [Board-and-Memory.md](Board-and-Memory.md#cache-is-carved-from-the-same-pool):
  code and data placement.
- [Debugging.md](Debugging.md#performance-seems-off): device measurements.
