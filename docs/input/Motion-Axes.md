# Motion axes

`input/motion/imu.c` configures the QMI8658 (+/-8 g, +/-512 dps) and reads all
six axes in one burst. Keep the burst intact: separate reads can straddle
sample updates. A stream FIFO holds up to `IMU_FIFO_SAMPLES` at the rate
`IMU_ODR_HZ`; see [Gravity.md](Gravity.md#update) for the drain.

The IMU's axes do not match the screen's:

| Screen direction | Sensor axis |
|---|---|
| down (+y) | `+ax` |
| right (+x) | `-ay` |

Use `imu_gravity_screen()` in `input/motion/imu_sample.h`, including its
orientation handling, rather than mapping sensor X to screen X. The
accelerometer measures gravity plus linear acceleration; the gyroscope
measures angular velocity, not tilt angle.

`input_read_motion()` (`input/input.h`) returns `input_motion_t` with one I2C
read per call:

| Field | Meaning |
|---|---|
| `acceleration` | offset-calibrated accelerometer, unfiltered, in screen axes (x right, y down, z out of the screen), pointing down at rest, Q12 g (`INPUT_ACCEL_ONE_G`). Upright reads +1 g on y; flat, screen up, -1 g on z |
| `rotation` | signed raw gyro counts, Q6 deg/s (`INPUT_ROTATION_ONE_DPS`), panel axes: x = -gy, y = gx, z = gz |
| `turn_speed` | deg/s, the sum of the absolute rates over all three axes: a speed with no sign or axis |

Rotation follows the right-hand rule on the screen axes: +z is clockwise from
the front, +x tips the bottom edge toward the viewer, +y turns the right edge
away. Calibrating the offsets is in [Motion-Calibration.md](Motion-Calibration.md).
