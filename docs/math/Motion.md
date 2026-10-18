# Motion

Orbit steering and spring-line motion with caller-owned state. [Math index](README.md).

## orbit_motion.h

A transform orbits a target, with direct steering, a decaying fling and a critically damped reset.

| Function | Purpose |
|---|---|
| `orbit_motion_state_t` | Yaw and pitch in radians, and natural log of distance |
| `orbit_motion_limits_t` | Pitch and log-distance bounds |
| `orbit_motion_t` | Target, current and home states, velocity, limits and time constants |
| `orbit_motion_input_t` | One frame of steering |
| `orbit_motion_init` | At home, still, with default half-lives; clamps home to limits |
| `orbit_motion_update` | Advances by seconds; held steering cancels a reset |
| `orbit_motion_pose` | Transform with +z facing the target and +x level |
| `orbit_motion_fit_distance` | Distance that fits a sphere in the lens with padding around each half-axis |

| Constant | Purpose |
|---|---|
| `ORBIT_MOTION_PITCH_LIMIT` | Steepest pitch, short of a quarter turn |
| `ORBIT_MOTION_FLING_HALF_LIFE_S` | Time for fling speed to halve |
| `ORBIT_MOTION_DRAG_VELOCITY_HALF_LIFE_S` | Half-life for speed to follow held steering |
| `ORBIT_MOTION_RESET_TIME_S` | Reset spring time constant |
| `ORBIT_MOTION_RESET_SETTLED` | Home distance and speed threshold for settling |
| `ORBIT_MOTION_FRAME_PADDING` | Fraction of each picture half-axis left clear |

## spring_line.h

A row of points on springs carries a wave along caller-owned Q16 pixel offsets and velocities.

<details><summary>Spring-line state and operations</summary>

| Function | Purpose |
|---|---|
| `spring_line_t` | Caller-owned arrays, active half-open range, tension, stiffness, damping and carried time |
| `spring_line_init` | Initializes the line with caller-owned arrays |
| `spring_line_at_rest` | Tests whether the active range is empty |
| `spring_line_scale` | Scales by a share out of 256 |
| `spring_line_wake` | Includes columns in the active half-open range |
| `spring_line_add_bump` | Adds a smooth bump falling to zero at its half-width |
| `spring_line_poke` | Flicks with Q16 pixels per tick; negative is up |
| `spring_line_nudge` | Moves the line by Q16 pixels and lets go |
| `spring_line_quiet` | Tests rest offset and velocity thresholds |
| `spring_line_trim` | Puts quiet columns at each end of the active range to rest |
| `spring_line_tick` | Advances velocities then offsets by one tick, with free ends |
| `spring_line_advance` | Runs ticks for the elapsed milliseconds, carrying the remainder; returns ticks run |
| `spring_line_apply` | Writes rest plus offset, reports changed columns, and returns furthest movement in whole pixels rounded up |

</details>

| Constant | Purpose |
|---|---|
| `SPRING_LINE_TICK_MS` | Simulated time per tick |
| `SPRING_LINE_MAX_TICKS` | Tick cap per frame; drops excess time |
| `SPRING_LINE_REST_OFFSET`, `SPRING_LINE_REST_VELOCITY` | Thresholds for putting the line to rest |
| `SPRING_LINE_MAX_OFFSET` | Maximum offset magnitude |
| `SPRING_LINE_OUT_SHIFT`, `SPRING_LINE_OUT_ONE` | Fractional bits and units per output pixel |
