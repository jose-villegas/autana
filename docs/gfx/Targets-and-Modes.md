# Targets and Modes

## Render targets

`gfx_render_target.h`: a picture drawn off the panel as a set of attachments, each
a per-pixel map with its own size per pixel. Colour comes first, in the panel's own
format, then depth, then any further map a renderer attaches.

Every attachment has the target's width and rows, so one row index finds a pixel in
all of them.

| Function | What it does |
|---|---|
| `gfx_render_target_bytes()` | the bytes every attachment takes, from each one's size per pixel |
| `gfx_render_target_carve()` | points each attachment at its part of one block |
| `gfx_render_target_window()` | the same picture's rows `[row0, row1)` |
| `gfx_render_target_row()`, `_color()`, `_depth()` | an attachment's first pixel of a row |

A target can be carved from a renderer's scratch block, or point its colour at a
band buffer and its depth at a band of its own. What an attachment means beyond its
size is its renderer's.

## Modes

Requested with `gfx_mode_enter()` from an app's `enter()`, released with
`gfx_mode_exit()` from its `exit()`. `gfx_mode_resolve()` (`gfx_mode.h`) turns
the request into a grant and is pure; `gfx_mode_enter()` also allocates.

| | Full framebuffer | Band ring | Indexed | Double framebuffer |
|---|---|---|---|---|
| Request | the default | `GFX_LAYOUT_BANDS` | `GFX_LAYOUT_INDEXED` | `GFX_LAYOUT_DOUBLE_FB` |
| App writes | pixels, anywhere | pixels, one band at a time | palette indices, `gfx_indexed_image()` | pixels, whole frame |
| Buffer | 322 KiB, PSRAM | 2 x `GFX_BAND_HEIGHT` rows, DMA RAM | grid_w x grid_h bytes, internal RAM | 2 x `GFX_FRAMEBUFFER_BYTES`, PSRAM |
| Who sends | present task | gfx, from the shell's frame loop | present task | present task |
| Sends | dirty cells, runs or strips | dirty bands, whole | dirty strips, whole | every strip, whole |
| Content kept between frames | yes | **no**: a band is gone once sent | yes | front only; redraw every back pixel |
| For | anything that redraws part of a frame | a full-redraw renderer | a cell grid with a palette | a full-redraw renderer |
| Used by | the launcher, any microui screen | engine self-tests | a frame that is a grid of palette indices | full-frame pixel rendering |

Double mode draws into the back buffer while the front is sent.
`gfx_present_begin()` drains the preceding send and swaps them;
`gfx_present_wait()` returns at once. Dirty tracking, interlace and
half-picture expansion are disabled.

Mode exit drains the send, keeps the last presented frame as the single
framebuffer, and frees the other. Readback also drains before reading the front.

- `gfx_mode_enter()` asserts the mode is `GFX_LAYOUT_FULL_FB`: modes do not nest.
- A failed allocation grants nothing: the returned mode is still
  `GFX_LAYOUT_FULL_FB`. Check the grant's `layout`, not the request's.
- Geometry is the panel's full size. Per-axis interlace request fields are
  granted but do not alter drawing; `gfx_set_interlace()` controls strip sends.
- `GFX_BAND_HEIGHT` is 16, 32 or 64 rows by Kconfig, default 32, and always
  divides `GFX_HEIGHT`. On the device, at every band height, the two band
  buffers alias `gfx_present.c`'s strip-bounce slots rather than allocating; a host
  build mallocs them.

### Layout costs

Each picture's frame cost on the board, from one build, in `FULL_FB`,
`DOUBLE_FB` and two `DOUBLE_FB` variants. In `_CLEAR_ON_SEND` core 1 clears
each sent frame, so the draw skips its clear; in `_CONTENDED` a send runs
during the whole draw, so both share PSRAM. Not run: the suite has no such pass.

The r3d scenes draw in `update()`; the boot animation draws a full frame in
`frame()`.

<!-- generated: gfx-layout-costs sha256=5f4317e1e75d4249a07f8a4e057e62dca99bcec8cb5a295406ed27ffb6d0c1be -->
![Frame cost by framebuffer layout](../images/render/gfx-layout-costs.png)

| Picture | `FULL_FB` | `DOUBLE_FB` | `DOUBLE_FB_CLEAR_ON_SEND` | `DOUBLE_FB_CONTENDED` |
|---|---|---|---|---|
| sponza-fitted-full, portrait | 43.1 | 48.1 (+12%) | 52.1 (+21%) | not run |
| sponza-fitted-full, landscape | 40.7 | 45.7 (+12%) | 49.4 (+21%) | not run |
| sponza, portrait | 42.6 | 47.6 (+12%) | 51.3 (+20%) | not run |
| sponza, landscape | 41.2 | 46.1 (+12%) | 49.7 (+21%) | not run |
| sponza-lite, portrait | 35.0 | 39.7 (+13%) | 43.6 (+24%) | not run |
| sponza-lite, landscape | 34.2 | 38.8 (+14%) | 42.6 (+25%) | not run |
| sponza-flat, portrait | 35.0 | 39.6 (+13%) | 43.7 (+25%) | not run |
| sponza-flat, landscape | 34.8 | 39.4 (+13%) | 43.4 (+24%) | not run |
| sponza-fitted, portrait | 34.0 | 39.0 (+15%) | 42.8 (+26%) | not run |
| sponza-fitted, landscape | 32.5 | 37.3 (+15%) | 41.1 (+27%) | not run |
| sponza-flat-fitted, portrait | 34.4 | 39.4 (+15%) | 43.4 (+26%) | not run |
| sponza-flat-fitted, landscape | 33.4 | 38.4 (+15%) | 42.2 (+26%) | not run |
| capybara, portrait | 13.7 | 18.4 (+35%) | 22.6 (+65%) | not run |
| capybara, landscape | 14.4 | 18.9 (+31%) | 23.0 (+60%) | not run |
| boot animation, curve_climbing_early | 31.6 | 24.2 (-23%) | 20.5 (-35%) | 23.9 (-25%) |
| boot animation, title_flying_in | 34.4 | 26.9 (-22%) | 22.3 (-35%) | 26.6 (-23%) |
| boot animation, grid_settled_pre_image | 27.1 | 19.6 (-28%) | 15.5 (-43%) | 19.2 (-29%) |
| boot animation, crossfade_start | 41.9 | 34.3 (-18%) | 29.2 (-30%) | 33.9 (-19%) |
| boot animation, crossfade_mid | 43.4 | 35.7 (-18%) | 30.5 (-30%) | 35.4 (-18%) |
| boot animation, crossfade_late | 44.0 | 36.4 (-17%) | 31.1 (-29%) | 36.1 (-18%) |
| boot animation, near_end | 22.2 | 14.5 (-35%) | 15.3 (-31%) | 14.4 (-35%) |

Board ms per frame, mean of each suite's samples. A change is against the first layout its row reports.
<!-- /generated: gfx-layout-costs -->

Regenerate with
[`render_doc_images.sh`](../../launcher/tools/render/render_doc_images.sh), from
the captures in [render/data/](../render/data/).

## Expanded frames

An expanded frame holds an exact-half picture in `gfx_half_picture()`: a buffer in
gfx's own PSRAM that costs 82 KB while a full-framebuffer app runs.

- `scene_compose()` copies into it and `scene_shell_compose()` calls
  `gfx_expand_frame()`; a paused or undrawn scene keeps its last picture.
- Presentation doubles it into send strips until `gfx_present_wait()` completes.
  Raw framebuffer drawing is guarded during that interval, except in double mode,
  which has no expanded frames.
- `ui_end()` bins commands instead of drawing into it. The shell queues its home
  hint before the app builds the UI; its frame overlay then replays the bin and the
  development build mark into each strip. Readback uses the same path.

## Indexed mode

The app writes bytes into `gfx_indexed_image()`, row-major, `index_grid_w` per row,
and marks the matching panel rectangle dirty; cell (cx, cy) covers `cell_size` x
`cell_size` panel pixels. It presents with the same `gfx_present_begin()` /
`gfx_present_wait()` as the default mode.

| Call | Installs |
|---|---|
| `gfx_indexed_set_lut()` | the 256-entry index -> RGB565 table |
| `gfx_indexed_set_lut16()` | the (index, Bayer phase) table for dithered 16-colour mode |
| `gfx_indexed_set_dither16()` | which of the two expands; both stay installed |
| `gfx_indexed_set_dither()` | the spatial dither pattern for 16-colour mode |

All four are safe only between frames.

The palette type lives in the [image package](../../launcher/packages/image/include/image/palette/palette.h).
Entries 0-15 are reserved for UI by `DRAW_PALETTE_UI_ENTRIES` in
[draw_palette_standard.h](../../launcher/main/draw/draw_palette_standard.h).

- Curated palettes ship in `draw/draw_palette_standard.h`, chosen at runtime by name.
- Building one is the app's work.
- The two steps every palette then needs, the colour -> index map and the dither
  table, are `tools/gen/draw_palette_gen.h`: host-only, in
  OKLab<sup>[[35]](../Citations.md#ottosson-2020)</sup>, never in the firmware image.
