# Image entries

Part of the [asset docs](README.md). A picture is one entry of type `IMAG`, read in place ([IMAG v1](formats/IMAG.md)).

`NAME.image.toml` names a PNG in its own folder (`source = "x.png"`) and how
many quarter turns clockwise take it to the stored pixels (`quarter_turns`, 0
to 3; a picture drawn in a landscape frame is turned into the panel's portrait
one once, here, so drawing it reads rows in order). The source must be opaque.

## Example

Find an image entry in an open pack and read one pixel. The pixels are the entry's own bytes, so nothing is copied.

<!-- generated: asset-image-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
/* Read a picture from a pack: the colour of one pixel of an RGB565 image entry. */
#include "draw/draw_image.h"
#include "gfx/target/gfx_color.h"

asset_status_t
image_example(const asset_pack_t* pack, const char* name, int x, int y, gfx_color_t* color) {
    asset_view_t entry;
    image_t image;
    asset_status_t status = asset_pack_find(pack, name, DRAW_IMAGE_ASSET, &entry);
    if (status == ASSET_OK) {
        status = draw_image_open(entry, &image);
    }
    if (status == ASSET_OK
        && (image.format != IMAGE_RGB565BE || x < 0 || y < 0 || x >= image.width || y >= image.height)) {
        status = ASSET_ERR_FORMAT;
    }
    if (status == ASSET_OK) {
        const gfx_color_t* row = image_row(&image, y);
        *color = row[x]; /* in place: no copy */
    }
    return status;
}
```
<!-- /generated: asset-image-example -->
