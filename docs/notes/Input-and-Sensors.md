# Input and Sensors

Part of the [platform notes](README.md). Sensor, bus and chip facts only; how
each input feature behaves is in [input/](../input/README.md).

## Touch controller

Both board revisions open through `board_touch_open()`. An idle controller can NACK register
reads, logging `i2c transaction failed` and `FT5x06 ... I2C read error!`; use
these as console search terms and do not poll blindly.

GPIO 21 is active-low data-ready, not a finger-down level. The CST820 pulses
it, the interrupt handler latches the report and wakes the polling task, and a
held contact keeps being read without another pulse. Delivery and calibration:
[Touch.md](../input/Touch.md).

## IMU (QMI8658)

| Fact | Value |
|---|---|
| Ranges | set in `input/motion/imu.c` |
| Rate and buffer | `IMU_ODR_HZ` with a stream FIFO of `IMU_FIFO_SAMPLES`, in `input/motion/imu.h` |
| Axes | not the screen's; [Motion-Axes.md](../input/Motion-Axes.md) |
| Calibration | offsets and gyro zero-rate: [Motion-Calibration.md](../input/Motion-Calibration.md) |

## Power management (AXP2101)

The PWR button is a latched PMU interrupt over I2C, not a GPIO level; BOOT is
GPIO 0. Registers and rules: [Buttons.md](../input/Buttons.md).

## Related

- [Board-and-Memory.md](Board-and-Memory.md): hardware inventory and buses.
- [Debugging.md](Debugging.md#orientation-or-the-imu-seems-wrong): raw readings
  and orientation snapshots.
