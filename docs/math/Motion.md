# Motion: steer2f.h

Steering for agents that walk a ground plane, after Reynolds.<sup>[[122]](../Citations.md#reynolds-1999)</sup>
A `steer2f_agent_t` is a position, a velocity and a heading; `steer2f_limits_t` holds its speed, acceleration and turn limits and the two radii.
A behaviour returns an acceleration, and one step applies it. [Math index](README.md).

| Function | Result |
|---|---|
| `steer2f_arrive` | desired velocity toward the target at `speed`, scaled by $`\min(1, d/r_a)`$ inside `arrive_radius`, minus the velocity; at most `max_accel` |
| `steer2f_separation` | $`\sum (p - p_j)/d_j \,(1 - d_j/r_s)`$ over neighbours nearer than `separation_radius`, times `max_accel`, at most `max_accel`; two agents on one spot part along x by index |
| `steer2f_avoid` | for each neighbour, the time $`t^*`$ their paths come closest; inside `horizon_s` and nearer than `separation_radius` there, a push apart along that offset, weighted $`(1 - d^*/r_s)(1 - t^*/T)`$; at most `max_accel` |
| `steer2f_contain` | zero inside the circle; outside, toward its centre, ramping to `max_accel` at `arrive_radius` past the edge |
| `steer2f_step` | velocity += clamped accel · dt, clamped to `max_speed`; position += velocity · dt; heading turns toward the velocity by at most `turn_rate` · dt, and holds below `heading_speed` |

Separation reacts to where agents are, avoidance to where they are going, so fast agents part before they meet.
Separation weights by $`1 - d/r_s`$ rather than the paper's $`1/r`$, so the push is bounded when two agents touch.
The heading is radians from +x toward +y, kept in $`[-\pi, \pi]`$; a still agent keeps the way it faced.
