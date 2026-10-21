# Buttons

The two buttons are different kinds of device.

| Button | Signal | Interpretation |
|---|---|---|
| BOOT | active-low GPIO 0 | level debounced into edges by `input/buttons/button_fsm.c`; also selects the ROM bootloader |
| PWR | AXP2101 latched interrupt status over I2C | completed event, not a readable finger-down level |

[power/](../../launcher/main/power/README.md) owns the PMU handle and key
interrupts. The input polling task calls `power_poll()` at `POLL_HZ`,
independently of rendering; its battery cache adds one refresh per second.

| PMU register | Rule |
|---|---|
| `0x41`, bits 3/2 | enable short/long-press interrupts |
| `0x49`, bits 3/2 | latched short/long-press status; write one to clear only consumed bits |
| `0x27` | independent long-press IRQ, power-off and power-on thresholds |
| `0x22`, bit 1 | enables button power-off; bit 0 selects power-off or restart |

Do not clear all interrupt bits: other latched power events may have
consumers. Firmware leaves power-off settings at their board defaults and logs
the decoded values at startup.

A long PWR hold can cut power. That is configurable PMU behaviour, not a GPIO
event. Recovery is in
[Flashing-and-Toolchain.md](../notes/Flashing-and-Toolchain.md#flashing-and-recovery).
