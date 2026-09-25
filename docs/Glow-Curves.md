# Glow curves

This drawing primitive lights pixels around a posed curve. For the draw targets and
presentation path, see [`Gfx-and-Presentation.md`](Gfx-and-Presentation.md).

`gfx_glow_curve_posed()` draws a curve as light: every pixel within a radius
is coloured by its true distance to the curve. The curve is a Q4 height per
column of a view frame posed in the panel. The arithmetic is in
`gfx/gfx_glow.h`; `gfx.c` adds guards, the target and dirty marking.

- **True distance.** The distance search checks the neighbouring curve spans
  so a steep slope lights only pixels near the curve.
- **No square root per pixel.** The search compares squared distances; the
  ramp index comes from one 256-byte table read at two scales.
- **A style is a baked ramp** (`gfx_glow_style_set()`): 64 steps of distance,
  once per cell of the 4x4 Bayer matrix. Each phase rounds the same 8-bit
  colour to RGB565 at a different threshold. Changing colour or radius
  rebuilds the ramp.
- **A stippled halo uses the same ramp.** `gfx_glow_style_set_stepped()` holds
  the light to equal levels and lets each phase decide the remainder. The
  launcher's ridge reads the step count from `ridge.glow_steps`.

`ui/ui_ridge.c` combines the glow with `util/spring_line.h`, which advances
one offset per column. When the spring is at rest, the ridge skips the draw.

The view frame has a *pose*: a Q14 unit vector pointing down on the panel,
so a gravity reading is a pose with no angle or arctangent in between. A
turned curve is no longer a height per panel column, so it walks panel rows
and asks of each pixel where it lies in the view frame - an add per pixel,
against a `gfx_glow_field_t` prepared once per change of the curve. It asks
only where light can fall. A row that crosses fewer than
`GFX_GLOW_FEW_COLUMNS` view columns - the curve running along the panel's
rows - is narrowed once, to the exact reach of those columns. Any other row
is walked in blocks of `GFX_GLOW_ROW_BLOCK` pixels, and a block is skipped
when the view positions of its two ends show that no column under it can be
lit: adds, shifts and compares against the reach of chunks of columns, since
a 64-bit division is a library call on this chip and one per block cost more
than the pixels it saved. It keeps, per panel row, the
stretch it lit, and blackens that before drawing the row again, so a curve
that turns needs nothing clearing behind it either.

What it does with that stretch is the `trail` argument: 0 blackens it, 255
leaves it - every place the curve has been stays lit, since the framebuffer
and the panel both simply keep what was written - and a value between dims
it to `trail`/256 per draw, a tail that fades. With a trail the draw keeps
whichever of old and new light is brighter: each band overlaps most of the
last, and would otherwise overwrite its bright core with a dim rim, leaving
only rim light behind.

The trail lives nowhere but the framebuffer itself: it is the light already
there, dimmed and drawn over, so whatever repaints the panel underneath it -
a UI that changed and asked for its backdrop again, a turn of the UI, any
full redraw - wipes it, and it has to grow back over the following draws. It
also needs a framebuffer to read, so a trail is not available in band mode.

A fading tail has to go on being drawn after the curve stops, or it freezes
there. For how long is counted, not watched for: `gfx_glow_trail_draws()` is
how many draws take the brightest colour to black at a given `trail`, so a
caller can schedule exactly that many more draws after the curve stops
rather than polling the tail's own rows for whether any pixel is still lit.
The launcher's ridge uses `ridge.trail`.

**A map of the light, so that drawing is a lookup.** Searching beside every
pixel costs in proportion to the radius, and so does the number of pixels, so
a wide glow cost its radius squared: from radius 13 to 31 the lit area grew
2.4 times and the work 4.3 times. The distance to the curve does not depend on
how it is turned, only on its shape, so a `gfx_glow_map_t` holds it, worked
out once per shape in the curve's own frame by an exact two-pass transform
(vertical distances, then the lower envelope of the parabolas they raise,
after Felzenszwalb and Huttenlocher) whose cost is the map's area whatever
the radius. A draw then reads four cells and blends them; turning the curve
costs no distance work at all. The map holds *squared* distance, which is
what the ramp is indexed by and which blends almost exactly, at one cell to
two pixels each way, since a glow is smooth. The line itself is not, so
within 5 px of it a draw still searches, over a window that small: there the
mapped picture is the searched one bit for bit, and around it they differ by
under half a percent of full light on average. Measured on a host, a mapped
frame costs the same at every radius; at 31 it is 3.7 times less work than
searching when the shape moved and 6.2 times when the curve only turned.
Under a radius of about 10 the map's fixed cost is more than the search it
saves, and the launcher's ridge switches by radius. The ridge's map is about
125 KiB, in PSRAM with the rest of its state.

None of this touches the other cost. Every lit pixel is still written and
sent, and that grows with the radius however the light is worked out.

The launcher's ridge is a horizon: it follows `input/tilt.h`'s down at any
angle while the app rows turn in quarters. It holds boot's landscape pose for
its first 700 ms, since boot knows no orientation, then eases to level.
Easing never quite arrives and a hand is never still, so the pose is redrawn
only once it is half a degree from the one on screen, and put exactly level
once down has held still for 300 ms.

It is also never quite still (`ui/ridge_motion.h`, pure and host-tested). It
**breathes**: every `ridge.breath_ms` its rest shape eases toward a smoothed
copy of the ridge and back to the rigid original. A **wave**
`ridge.wave_height` (sixteenths of a pixel) high runs along it. And the
wave has **momentum**: while the device turns, the line lags true
level, so for that moment the ridge is a slope - the sine of the lag - and
the wave is pushed down it and coasts on after. All three come in over 4 s
after the line is released (`ridge.ambient_ease_ms`), slowly at first and
slowly into full, so the stiff line loosens rather than starts; at the
hand-over from boot the line is rigid, on the photograph. The price is that the launcher draws every frame, and a frame
in which the ridge moves is close to a full send. `ui_ridge_set_ambient()`
turns it off, which previews do: without it the launcher is idle whenever it
is untouched and level.

Cost follows lit pixels: Cerro Autana's ridge at radius 13 lights about
16,600 of them. A curve spanning the view touches most bands at any pose, so
a frame in which it moves is close to a full send.

## Related

- [`Gfx-and-Presentation.md`](Gfx-and-Presentation.md) - draw targets, dirty tracking, and presentation
- [`tools/Live-Tuning.md`](tools/Live-Tuning.md) - tuning the ridge on a running device
