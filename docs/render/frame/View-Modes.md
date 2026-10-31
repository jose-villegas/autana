# View Modes

Development builds select one row from the `render_debug_view_t` table in
`render/context/render_context.c`: depth, tiles, motion, meshlets, cull or boxes.

Every view, drawn at the same pose:

| View | Looks like | The colour buffer becomes |
|---|---|---|
| shaded | ![The shaded view](../../images/render/sponza-view-shaded.png) | untouched: the baked colours as drawn |
| depth | ![The depth view](../../images/render/sponza-view-depth.png) | the depth as a grey ramp, nearest white and farthest black |
| tiles | ![The tiles view](../../images/render/sponza-view-tiles.png) | the same ramp, each `RASTER_SHOW_TILE` square at its farthest depth |
| motion | ![The motion view](../../images/render/sponza-view-motion.png) | mid-grey for none, red for x and green for y |
| meshlets | ![The meshlets view](../../images/render/sponza-view-meshlets.png) | a flat hue for each cluster, with disjoint IDs for each instance |
| cull | ![The cull view](../../images/render/sponza-view-cull.png) | each cluster in the colour of its fate this frame; see [The cull, seen](Culling.md#the-cull-seen) |
| boxes | ![The boxes view](../../images/render/sponza-view-boxes.png) | untouched; its overlay draws each instance's box as the frame placed it, one hue per mesh, for meshes drawn at least `boxview.copies` times; edges behind the drawn depth hidden, dimmed or full by `boxview.behind` |

The depth and depth-tile views along a flythrough:

![The depth view along a flythrough](../../images/render/sponza-depth.gif)
![The depth-tile view along a flythrough](../../images/render/sponza-tiles.gif)

| Call | What it does with the row |
|---|---|
| `render_context_set_debug_view()` | Attaches it and owns its zeroed PSRAM state; switching frees the previous state |
| `RENDER_DEBUG_VIEW_SHADED` | Detaches it |
| `render_context_release()` | Frees its state |
| `render_context_debug_view()` | Exposes its name and constructor to tools |

```mermaid
flowchart LR
    Render["the draw<br/><i>colour and depth</i>"] --> Show
    Show["raster_show()<br/><i>the attached show hook<br/>repaints colour only</i>"] --> Upscale["raster_upscale()<br/><i>into the destination</i>"]
```

A row may also carry two optional hooks:

| Row hook | What it does |
|---|---|
| `second_view` | After the frame's draw and after each draw it asks for, may name another view and fill the census list with the clusters to draw through it, and its depth scale; the frame's stats stay those of the first draw |
| `overlay` | Draws lines over the composed picture; the shell then composes at full size, not into a half picture |

Overlays draw with `render/view/raster_lines.h`: segments and boxes in a lens's space, cut at the near plane, optionally dimmed or left out where the drawn depth is nearer.

- The ramp is stretched over the range this frame drew: the most detail
  within a frame, not comparable between frames.
- A pixel nothing drew takes the raster's `clear`, the colour upscaling gives
  it, so it reads as empty in every view; a tile holding one such pixel is
  empty.
- The views are at the raster's own size, before upscaling.
- `r3d_span.h` defines the depth encoding they read.
