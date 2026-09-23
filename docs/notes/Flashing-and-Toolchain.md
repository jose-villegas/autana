# Flashing and Toolchain

Part of the platform notes for the Waveshare ESP32-S3-Touch-AMOLED-1.8 - see
[`README.md`](README.md) for the full set.

---

## Flashing and recovery

**The chip only accepts auto-reset while an app is actively running.** Once
firmware returns from `app_main` and goes idle, reset signalling stops working
entirely — `Hard resetting via RTS pin` does nothing, esptool reports
`No serial data received`, and manual DTR/RTS pulses produce zero bytes.

This is why the launcher's frame loop never exits, and why its error paths park
in a sleep loop rather than returning from `app_main`: the device has to stay
flashable even when startup fails.

If the board becomes unreachable, BOOT has to be held at the moment power
arrives - so what produces that moment decides the procedure.

**No battery fitted**, where unplugging USB really does remove power:

1. Unplug USB-C
2. **Hold BOOT**
3. Plug USB-C back in while still holding
4. Keep holding ~2 s, release

**With a battery fitted, that sequence cannot work**, and it fails silently
rather than reporting anything: the AXP2101 keeps the rail up from the battery,
so unplugging USB never power-cycles the SoC and step 3 delivers no power-on at
all. The chip carries its stuck state through every replug. Power off through
the PMU instead, which is the only thing that cuts a battery-backed rail:

1. **Long-press PWR** (~10 s) until it powers off - the COM port disappearing
   is what proves the rail actually dropped; without that, this step did nothing
2. **Hold BOOT**
3. **Press PWR** to power on, still holding
4. Keep holding ~2 s, release

That forces the ROM bootloader regardless of firmware state. Confirm you are in
download mode with:

```bash
esptool.py --chip esp32s3 -p <PORT> --before no_reset flash_id
```

Connecting almost instantly (a few dots) means the chip is sitting in the
bootloader.

**After flashing this way, the board will not boot on its own** — `--after
hard_reset` uses the same non-functional RTS reset, so it stays in download
mode, silent, running nothing. **`--after watchdog_reset` starts it from
there**, tripping the SoC's own watchdog instead of the RTS line, and needs
nobody at the bench - verified from the ROM bootloader on this board, which
`hard_reset` cannot restart. A power cycle works too, but with a battery that
means the PWR sequence above rather than a replug.

A restart re-enumerates USB Serial/JTAG, and Windows may hand the board a
**different COM number** than it had before. Anything holding a port by name
breaks there; `find_port()` in `scripts/device/device.py` looks it up by
vendor id `0x303A` each time for that reason.

If it vanishes from USB entirely — no COM port, no device at vendor ID
`0x303A` — check
the cable first, then the PWR button: this board's power is managed by an
**AXP2101 PMIC**, so a long press cuts system power.

---

## Toolchain

- **ESP-IDF v5.5+ is required.** The Waveshare BSP declares `idf: ">=5.5"`;
  v5.4 will not resolve it. Both can coexist — they are keyed by `IDF_PATH`.
- **What the build scripts need is a working `export.bat`, not just a working
  `idf.py`.** `launcher/tools/idf.sh` runs ESP-IDF from Git Bash by handing
  the command to `cmd`, because v5.5 refuses to activate under MSYS at all.
  Espressif's newer `eim` installer satisfies `idf.py` and not this: it emits
  only a PowerShell activation script, and it clones via libgit2, so
  `export.bat` arrives with LF endings and `cmd` cannot resolve a batch label
  in one. Its Python environment is also somewhere `export.bat` does not
  look. Running ESP-IDF's own `install.bat` against that same checkout adds
  what is missing and re-downloads no toolchain.
- **`IDF_TOOLS_PATH` is the root, not the `tools/` inside it.** Point it one
  level too deep and `idf_tools.py` installs a second copy of every toolchain
  under `tools/tools/`.
- BSP component: `waveshare/esp32_s3_touch_amoled_1_8` `^2.0.3` (see
  `launcher/main/idf_component.yml`), plus the two panel drivers it only
  depends on privately and so must be declared again here directly:
  `espressif/esp_lcd_co5300` (V2) and `waveshare/esp_lcd_sh8601` (original).
- `sdkconfig.defaults` worth keeping: `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y` —
  without it the image header says 2 MB and the bootloader warns on every boot.

Console output reaches the USB CDC port because
`CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` makes USB-Serial-JTAG the *primary*
console outright — this board's one USB-C port is the SoC's own native
USB-Serial/JTAG peripheral, not an external USB-UART bridge on UART0.
ESP-IDF's own default assumes the other, more common board design (UART0
primary, USB-Serial-JTAG a write-only secondary mirror), which would leave
input silently unread on a board wired this way; see
[Diagnostics-and-Debugging.md](Diagnostics-and-Debugging.md) for the full
mismatch this fixes.

---

## The build flag and the frame tick

`CONFIG_COMPILER_OPTIMIZATION_PERF` (-O2) is set in `sdkconfig.defaults`,
the right choice for a device whose every frame is rasterising, cellular
automata and pixel loops - there is no debugger attached to this board to
trade away for it. A build directory's generated `sdkconfig` is not
re-derived from `sdkconfig.defaults` just because the defaults changed.
`tools/idf_variant.sh` deletes one that is older than a fragment it was built
from, so change the defaults and rebuild through `tools/build_flash.sh`
rather than trusting a directory left over from before.

The frame loop ends in `vTaskDelay(1)` (`main.c`), so frame time is work
rounded up to a whole tick - compare microseconds of work, not an fps
figure, which quantises around any small change.

---

## Related

- [Display-and-Rendering.md](Display-and-Rendering.md) — the render-path
  numbers these build settings affect.
