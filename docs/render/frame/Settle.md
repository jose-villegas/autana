# Settle

`render/settle/render_settle.h`: quiet rows of a held picture refine after
[partial redraw](Partial-Redraw.md) draws the live changes. A held view is
never [generated](Frame-Generation.md).

| Live choice | Refined value |
|---|---|
| `texture.divide_px` | 1: divide at every pixel |
| `texture.affine_bend` | 0 |
| `texture.affine_level` | 0 |
| `texture.far_level` | 0: keep texturing |
| `small_fill.ceiling` | `SMALL_FILL_CEILING_OFF` |

The raster height is split into `RENDER_SETTLE_BANDS` bands. Band `b` covers
`[b * height / BANDS, (b + 1) * height / BANDS)`. Each held frame draws one
contiguous run of quiet bands that have not yet refined.

The first due band is picked middle out: middle, above, below, then outward.
The run grows around it while the budget buys more rows. At least one band
is drawn once due, even when its cost exceeds the budget.

| Setting | Meaning |
|---|---|
| `settle.frame_us` | Refine budget per held frame; 0 disables settle |
| `settle.hold_frames` | Quiet frames before a band is due |

Defaults and pack ranges are in [Render-Settings.md](../Render-Settings.md).
A refined row's measured cost moves the estimate by
`1 / RENDER_SETTLE_COST_WEIGHT_DIVISOR` per sample.

| Event | State |
|---|---|
| Live rows touch a band | Quiet count and refined flag reset |
| Held frame leaves a band alone | Quiet count rises, saturating at `RENDER_SETTLE_HOLD_MAX` |
| View is not held, budget is zero, or live quality already matches refined | Every band and the cost estimate are forgotten |
| Timed or generated frame | Every band and the cost estimate are forgotten |

Refinement restores the live settings before keeping the picture. Its time
is recorded separately from live draw time, so dynamic resolution judges
held-frame live work alone.
