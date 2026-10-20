# Buttons

The two buttons are different kinds of device.

| Button | Signal | Interpretation |
|---|---|---|
| BOOT | active-low GPIO 0 | level debounced into edges by `input/buttons/button_fsm.c`; also selects the ROM bootloader |
| PWR | AXP2101 latched interrupt status over I2C | completed event, not a readable finger-down level |

`input/buttons/buttons.c` enables the PMU's short-press interrupt and polls
independently of rendering. Its `POLL_HZ` controls shared-bus traffic.

| PMU register | Rule |
|---|---|
| `0x41`, bit 3 | enables the short-press interrupt |
| `0x49`, bit 3 | latched short-press status; write one to clear only the consumed bit |
| `0x27` | independent long-press IRQ, power-off and power-on thresholds |
| `0x22`, bit 1 | enables button power-off; bit 0 selects power-off or restart |

Do not clear all interrupt bits: other latched power events may have
consumers. Firmware leaves power-off settings at their board defaults and logs
the decoded values at startup.

A long PWR hold can cut power. That is configurable PMU behaviour, not a GPIO
event. Recovery is in
[Flashing-and-Toolchain.md](../notes/Flashing-and-Toolchain.md#flashing-and-recovery).
