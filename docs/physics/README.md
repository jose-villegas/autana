# Physics: springs

The [physics package](../../launcher/packages/physics/README.md) holds state that
moves because forces push it: each step integrates springs, pressure or gravity
over caller-owned arrays. It builds on [math](../math/README.md) and nothing else,
allocates nothing, and every header is checked by a host suite.

## Pages

| Page | Holds |
|---|---|
| [Spring](Spring.md) | A row of points on springs carrying a wave |
