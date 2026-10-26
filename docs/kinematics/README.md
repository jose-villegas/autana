# Kinematics

The [kinematics package](../../launcher/packages/kinematics/README.md) holds
state moved by prescribed inputs: it follows a target, a curve or a steer in
closed form, with no mass and no forces. It builds on [math](../math/README.md)
and nothing else, allocates nothing, and every header is checked by a host suite.

## Pages

| Page | Holds |
|---|---|
| [Follow](Follow.md) | Exact steps toward a target: halving decay, a coast and a critically damped spring |
| [Orbit](Orbit.md) | A transform orbiting a target: steering, fling, reset and framing a sphere |
