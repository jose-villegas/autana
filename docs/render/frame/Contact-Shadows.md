# Contact Shadows

A skinned mover casts no shadow, and the [light probes](Light-Probes.md)
that light it do not see it, so nothing darkens the ground under it. A
contact disc stands in: a dithered dark disc on the ground below the mover,
drawn after the geometry.

![The dithered disc at strengths 0.25, 0.5, 0.75 and 1, then the share of pixels halved against distance from the centre, drawn and modelled](../../images/render/contact-disc.png)

## Placing a disc

[`scene_set_contact_blob(renderer, ground_y, strength)`](../../../launcher/main/scene/scene.h) puts a disc on the
plane `y = ground_y`, under the centre of the renderer's world bounds, with
a radius of the bounds' smaller horizontal half extent. A strength of 0
turns it off; `SCENE_CONTACT_BLOB_STRENGTH_DEFAULT` is 0.75.

## Drawing it

The disc draws after the geometry, against its depth, so walls in front
hide it. A pixel at distance $`r`$ (over the radius) is halved in each
RGB565 channel when its 4×4 Bayer threshold $`t`$ passes
$`t/16 < s\,(1 - r^2)^2`$, with $`s`$ the strength.

Coverage so falls smoothly to zero at the rim, in 16 steps (the figure's
lower plot). Rows are bounded by the disc's projected outline, clipped at
the near plane, and the coverage test needs no per-pixel divide. Partial
redraw covers the disc's old and new bounds with the renderer's.

## Cost and goals

The discs run in `r3d.draw`, as the raster's post draw, and are not timed
apart from it, so one disc's cost is not measured. The contact stress
preset's goals, in microseconds per frame, each the worst of several board
captures plus 1%; quarter is the panel's orientation, each stage a
[frame-cost](../../tools/Frame-Cost.md) bracket:

<!-- generated: contact-cost check: python launcher/main/apps/render_lab/tools/stress_doc_table.py docs/render/frame/Contact-Shadows.md contact-cost stress-contact-blobs --check -->
| Preset | Quarter | `r3d.cull` | `r3d.transform` | `r3d.draw` | `r3d.upscale` | `frame.record` | Sum goal | Ceiling |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| `stress-contact-blobs` | portrait | 190 | 720 | 4160 | 2180 | 10 | 7190 | 8270 |
| `stress-contact-blobs` | landscape | 200 | 700 | 7320 | 2210 | 10 | 10320 | 11870 |
<!-- /generated: contact-cost -->

## Limits

- The disc lies on one flat plane at the height its caller gives.
- It is round whatever the mover's shape, and ignores the light's direction.
- It darkens by halving, so it shows the same on a lit floor and a dark one.

## Figures

[`contact_figures.py`](../../../launcher/tools/r3d/contact/contact_figures.py)
draws the figure from
[`contact_figures_probe.c`](../../../launcher/tools/r3d/contact/contact_figures_probe.c),
which calls the runtime's `contact_blob_draw()`.
