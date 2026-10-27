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
