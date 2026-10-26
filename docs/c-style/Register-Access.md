# Register access

How first-party C touches hardware. Part of the [C style guide](README.md).

| Level | Use | Where in the tree |
|---|---|---|
| ESP-IDF driver (`gpio_config`, `i2c_master_*`, `esp_lcd_*`) | the default | [`board_esp32s3.c`](../../launcher/main/board/board_esp32s3.c) |
| HAL low-level (`hal/*_ll.h`) | a field the driver does not expose | [`board_usb.c:45`](../../launcher/main/board/board_usb.c#L45) |
| Peripheral struct (`USB_SERIAL_JTAG.conf0.val`) | save and restore a whole register around a driver call | [`board_usb.c:41`](../../launcher/main/board/board_usb.c#L40) |
| Raw address or `REG_READ` / `REG_WRITE` | none today | |

## Rules

| Rule | Why |
|---|---|
| Hardware access lives under `launcher/main/board/` or the module that owns the device; everything above it calls a function | the host build compiles without a chip, and one place to fix a register mistake |
| Read the register once into a local, write it back once | a volatile struct read twice can change between reads |
| Name the reason above any access that bypasses the driver, and say what breaks without it | the driver is the supported path; a bypass is a debt |
| Pair a bypass with a link-time or `_Static_assert` check on the driver it steps around | [`board_usb.c:26`](../../launcher/main/board/board_usb.c#L31) asserts the wrapped function's signature |
| Code that must run with the flash cache off is `IRAM_ATTR` | see [Function attributes](Function-Attributes.md) |

## Wrapping a driver function

`board_usb.c` replaces ESP-IDF's PSRAM tuning with a function that drops the USB pull-up,
calls the real one, and restores the register. The pieces:

```mermaid
flowchart LR
    D["ESP-IDF PSRAM driver"] -->|"calls"| W["__wrap_mspi_timing_psram_tuning"]
    W -->|"save conf0, pull-up off"| R["__real_mspi_timing_psram_tuning"]
    R -->|"return"| W
    W -->|"restore conf0"| D
```

| Piece | Where |
|---|---|
| `--wrap=` link option, only for the 120 MHz PSRAM build | `launcher/main/CMakeLists.txt` |
| `__real_` declaration and the signature `_Static_assert` | [`board_usb.c:24`](../../launcher/main/board/board_usb.c#L28) |
| Link check that the wrap took effect | `launcher/tools/build/check_psram_tuning_wrap.py` |
