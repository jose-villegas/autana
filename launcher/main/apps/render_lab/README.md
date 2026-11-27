# Render Lab

A development-only lab for the renderer: software-rendered mesh scenes behind
one HUD and scene picker. Each scene has a key, and `render scenes` lists them.

| Console command | Result |
|---|---|
| `render scenes` | One `RENDER scene=<key> name=<name> current=<0\|1>` line per scene, then `RENDER_END` |
| `render scene <key>` | Switches to that scene: `RENDER scene=<key>`, then `RENDER_END`; `RENDER_ERR` for an unknown key |
| `render pose <ms>` | Stops the scene's camera path at `<ms>` along it: `RENDER pose=<ms>`, or `RENDER_ERR` with no path |
| `render pose play` | Plays the path again: `RENDER pose=play` |
| `render fly <right> <up> <forward> <drag x> <drag y> <wheel>` | Steers the camera for one step, the three axes each -1, 0 or 1; silent, so autana prints `sent`. Flight ends on its own after a second with no line (`tools/fly.py` sends these) |
| `render fly pose` | The flown camera: `RENDER fly position=... rotation=... speed=...`, or `RENDER fly=none` when not flying |
| `render fly release` | Gives the camera back to the scene: `RENDER fly=release` |

Send them with autana like any console line:
[`autana render scenes`](../../../../docs/cli/Drive.md#any-console-line).

`render scene` asks for the switch, which lands on a later frame. Wait until
`render scenes` reports `current=1` for the key before relying on it.

Words the lab does not know go to the showing scene's own console, if it has
one.
