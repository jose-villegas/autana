# Overdraw

The overdraw view paints each pixel by how many triangle spans tested it, on a
fixed ramp from blue (one layer) to red (`RASTER_OVERDRAW_RAMP_LAYERS` or more).
Bistro in landscape, at the same poses as its [cull sheet](Culling.md#the-cull-seen):

![Bistro's overdraw along its path](../../images/render/overdraw-bistro-sheet.png)

## Reading it

The view ([raster_overdraw.h](../../../launcher/main/render/view/raster_overdraw.h))
counts, per pixel, the spans that reached it and the spans that won its depth
test, and paints the first count.

- A cluster the cull drops never reaches a pixel, so it adds nothing here.
- A hidden cluster (red in [the cull view](Culling.md#the-cull-seen)) adds
  tested layers but no written ones.
- The frame totals (pixels, layers tested and written, the most at one pixel)
  are kept for a caller to print, on the line the header defines.

## What a layer costs

The cull walk visits clusters roughly near to far, so most extra layers fail the
depth test at once and cost a compare, not a shaded pixel. A frame's time
therefore follows the triangles set up
([What a cluster and a triangle cost](Culling.md#what-a-cluster-and-a-triangle-cost))
more than its layers.
