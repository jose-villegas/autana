# Input

Input features owned by `launcher/main/input/`, with their update rules,
constants and limits. Sensor, bus and chip facts are in the
[platform input notes](../notes/Input-and-Sensors.md).

| Reference | Scope |
|---|---|
| [Touch.md](Touch.md) | touch edge delivery, release rule, panel calibration and targets |
| [Buttons.md](Buttons.md) | BOOT and PWR signals, PMU registers |
| [Motion-Axes.md](Motion-Axes.md) | IMU axes against the screen's, `input_motion_t` fields |
| [Motion-Calibration.md](Motion-Calibration.md) | six-pose and unplugged calibration rounds, offset storage, measured captures |
| [Tilt.md](Tilt.md) | direction filtering, magnitude gate, shake strength |
| [Gravity.md](Gravity.md) | gyro prediction, acceleration trust and rejection, recovery and gyro bias learning |
