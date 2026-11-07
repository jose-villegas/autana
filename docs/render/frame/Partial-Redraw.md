# Partial Redraw

`render/partial/render_partial.h`: while the camera holds still, a frame
redraws and recomposes only the rows where something changed. The rest of the
picture stays as the last frame left it, and the result is the same picture a
full redraw gives.

It is on with the `partial_redraw` render setting
([Render-Settings.md](../Render-Settings.md#partial_redraw)).

```mermaid
flowchart LR
    Boxes["changed boxes<br/>where each object was and is"] --> Same{"same view, size,<br/>settings, instances?"}
    Same -->|"no"| Every["every row"]
    Same -->|"yes"| Rows["the rows the boxes cover<br/>through the view"]
    Rows -->|"none"| Kept["nothing drawn<br/>or composed"]
    Rows --> Draw["draw those rows"] --> Scale{"depth scale<br/>moved?"}
    Scale -->|"yes"| Every
    Scale -->|"no"| Up["upscale those rows,<br/>one more each side for a filter"]
```

## What it is told

The caller gives the next frame's changed boxes in world units with
`render_context_set_changed()`, each the union of where an object was drawn
and where it is now. A negative count means anything may have changed.

## When every row is drawn

| Cause | Why |
|---|---|
| A new view, size, clear colour or instance count | the kept picture is of something else |
| Any render setting changed | settings may change pixels anywhere |
| A debug view | it painted its own colours over the picture |
| Dynamic resolution or frame generation on | each frame may draw at another size or be warped |
| More than `RENDER_PARTIAL_CHANGES_MAX` boxes, or a negative count | the changes are not known |
| A box corner at or behind the near plane | its rows cannot be bounded |
| The draw refitted its depth scale | kept rows hold depth at the old scale |

A still instance's level picks ([Cluster-LOD](../../plans/Cluster-LOD.md))
do not change under a still view, so they never change a row no box covers.

## Rows

A box's rows are its eight corners projected through the frame's lens, plus
`RENDER_PARTIAL_ROW_MARGIN` rows each side for a vertex snapped to the next
row and a small fill's spill. The draw splits only those rows between the two
cores, and the upscale rewrites the destination rows they reach.

`render_context_frame()` reports what a frame recomposed: `unchanged` when it
drew nothing, else destination rows `[rows_first, rows_end)`.
