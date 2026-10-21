# Three Rules That Shape Everything

## 1. There is exactly one framebuffer by default

368 × 448 × 2 bytes = **322 KiB**, allocated in PSRAM
(`MEMORY_PSRAM` in `core/memory.h`), so it does not count against the
internal heap (see [Board-and-Memory.md](../notes/Board-and-Memory.md)).

`GFX_LAYOUT_DOUBLE_FB` adds a second, back framebuffer while it is on, so a
front is sent while a back is drawn.
See the [presentation memory policy](../gfx/Present.md#presentation-memory-policy)
and [transfer payload](../notes/Display-and-Rendering.md#transfer-payload).

"One framebuffer" is really "one destination at a time": an app may ask at
`enter()` for a band ring (a few strips of rows, sent as each fills) or an
index image (one byte per pixel through a palette), and gfx frees the
framebuffer while it holds one. See
[Gfx](../gfx/README.md).

The same rule is why the span rasterizer (`render/r3d_span.h`) owns no
framebuffer: it fills a caller's window of rows, with a depth plane only as
tall as that window. A full colour+depth pair would want ~1.3 MB here.
`math/linear/` supplies the float vector, quaternion, matrix and transform maths
`render/` and boot share, and no rasterizer.

## 2. There is exactly one frame loop, and it belongs to the shell

Apps do not loop, do not present, do not block and do not yield. An app's
`frame()` draws and returns. So:

- switching apps is instant: no teardown of a running loop
- no app can wedge the device by forgetting to yield
- the shell decides when to present, and can draw its own chrome afterwards

The boot animation runs a loop of its own and `show_post_failures()`
blocks for 8 s, but both run *before* this one exists. The rule is
about apps: the shell has to stay able to switch away from one, and at boot
there is nothing to switch to yet.

## 3. Apps are callbacks, not processes

One binary, one address space. There is no isolation: a misbehaving app can
corrupt the shell, and nothing stops a task on either core from touching
another app's state. That is the accepted trade for instant switching and no
flash-partition machinery. Worth revisiting only if third-party apps ever
become a goal.
