# Frame cost

`util/frame_cost.h` measures named stages of a frame. For how drawn pixels
reach the panel, see [`../Gfx-and-Presentation.md`](../Gfx-and-Presentation.md).

**Where a frame's time goes.** `util/frame_cost.h` brackets a stage of a frame
and charges its microseconds to a name:

```c
FRAME_COST_BEGIN(began);
draw_the_thing();
FRAME_COST_END(began, "thing.draw");
```

Every 1.5 s, under the shell's fps line, the console prints each name's
average milliseconds per frame over the window and the worst single bracket,
then the total those averages add up to (the numbers below show the line's
shape and are not measurements):

```
ms/frame avg/worst: ui.build 0.41/0.6  ridge.light 2.10/2.4  ridge.draw 3.02/4.1  ui.paint 1.20/9.8  present 8.31/16.6  frame.rest 0.90/1.4 | total 15.94
```

Charges are exclusive: a bracket nested inside another - `ridge.draw` inside
`ui.paint` on a frame where the UI changed - is taken out of the outer one,
so nothing is counted twice. `frame.rest` is one whole-iteration bracket
around the frame loop's body; being exclusive of everything else, it is
whatever no other bracket claimed. A stage that did not run in a window is
not listed. A bracket is two clock reads, about a microsecond each: around a
stage, never around a pixel. There are `FRAME_COST_SLOTS` (12) names; a
report flags a further one with ` +N dropped`, and a charge from any task but
the frame loop's own with ` +N foreign`. On a host and in release the
brackets compile to nothing.

## Related

- [`../Build-Variants.md`](../Build-Variants.md) - development and release instrumentation
- [`../Launcher-Architecture.md`](../Launcher-Architecture.md) - the shell frame loop
- [`../Gfx-and-Presentation.md`](../Gfx-and-Presentation.md) - gfx send counters and overlays
