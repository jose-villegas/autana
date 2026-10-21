# Three Rules That Shape Everything

## 1. There is exactly one framebuffer by default

368 × 448 × 2 bytes = **322 KiB**, allocated in PSRAM (`MEMORY_PSRAM` in
`core/memory.h`), so it does not count against the internal heap
([Board-and-Memory.md](../notes/Board-and-Memory.md)).

"One framebuffer" is really "one destination at a time":

| Destination | How an app gets it | Effect on the framebuffer |
|---|---|---|
| band ring | asks at `enter()`: a few strips of rows, sent as each fills | gfx frees it while the ring is held |
| index image | asks at `enter()`: one byte per pixel through a palette | gfx frees it while the image is held |
| `GFX_LAYOUT_DOUBLE_FB` | asks at `enter()` | adds a back buffer, so a front is sent while a back is drawn ([memory policy](../gfx/Present.md#presentation-memory-policy), [transfer payload](../notes/Display-and-Rendering.md#transfer-payload)) |

The modes are in [Gfx](../gfx/Targets-and-Modes.md#modes).

The span rasterizer (`render/r3d_span.h`) follows the same rule and owns no
framebuffer: it fills a caller's window of rows, with a depth plane only as tall
as that window, where a full colour+depth pair would want ~1.3 MB here.

`math/linear/` supplies the vector, quaternion, matrix and transform maths that
`render/` and boot share, and no rasterizer.

## 2. There is exactly one frame loop, and it belongs to the shell

Apps do not loop, present, block or yield. An app's `frame()` draws and returns,
so:

- switching apps is instant: no teardown of a running loop
- no app can wedge the device by forgetting to yield
- the shell decides when to present, and can draw its own chrome afterwards

The rule is about apps. The boot animation runs a loop of its own and
`show_post_failures()` blocks for 8 s, but both run *before* the shell's loop
exists, when there is nothing to switch to.

## 3. Apps are callbacks, not processes

One binary, one address space, no isolation: a misbehaving app can corrupt the
shell, and nothing stops a task on either core from touching another app's
state. That is the accepted trade for instant switching and no flash-partition
machinery; revisit it only if third-party apps become a goal.
