# Dirty Tracking

## Dirty tracking

`gfx_dirty.h`: inline functions over one tracker defined in `gfx_present.c`;
draw reaches them through `gfx_mark_dirty()` and `gfx_mark_rows_dirty()`. One tracker serves all three modes.

Two ways in. `dirty_mark()` takes a real box and may narrow a cell; the
rect and blit primitives use it, so a glyph dirties the glyph. `mark_band()`
takes rows only and has to claim every column at full width: what
`draw_pixel()` is left with it; `draw_clear()` marks the whole grid.

```
         92 px (COL_WIDTH)
        <----->
      +-------+-------+-------+-------+  ^
      | cell  |       |       |       |  | 64 rows (STRIP_HEIGHT)
      +-------+-------+-------+-------+  v
      |       | +-+   |       |       |      each cell keeps the BOX actually
      +-------+-+-+---+-------+-------+      dirtied inside it, not just a bit
      |  ...  7 strips x 4 columns    |
      +-------+-------+-------+-------+      under each cell: 4 x 4 leaves,
                                             23 x 16 px, one bit each
```

| Level | Size | State | Set by |
|---|---|---|---|
| strip | 368 x `STRIP_HEIGHT` (64), `STRIP_COUNT` = 7 | - | - |
| cell | `COL_WIDTH` (92) x 64, `GRID_COLS` = 4 per strip | one bit + a box | every mark |
| leaf | `LEAF_W` (23) x `LEAF_H` (16) | one bit | a real box only, never `mark_band()` |

| Caller | What it must do |
|---|---|
| any `gfx_*` draw call | nothing: it marks what it touched |
| `draw_clear()` | nothing: marks the whole screen |
| writes through `gfx_framebuffer()` or `gfx_indexed_image()` | **`gfx_mark_dirty()` the rectangle**: a miss looks like a frozen region, not a crash |
| static overlay content | ask `gfx_region_dirty()`; if false, skip the draw and the send |

## Repaint controls

| Call | Effect |
|---|---|
| `gfx_mark_all_dirty()` | send everything next present |
| `gfx_invalidate()` | next band frame forces every band |
| `gfx_request_full_redraw()` | both of the above, plus a pending flag the shell answers with the app's `invalidate()` |
| `gfx_set_interlace()` | alternate strips on alternate presents; skipped strips stay dirty. Off by default, RGB565 full framebuffer only. |
