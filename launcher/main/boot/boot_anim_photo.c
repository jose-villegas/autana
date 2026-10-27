/*
 * boot_anim_photo: the photograph the startup animation crossfades to, read
 * from the boot picture's own pack, one IMAG entry baked from boot.png.
 *
 * It is drawn as one full-panel copy, so a picture of any other size or
 * format is refused like a missing one: the animation then draws on without it.
 */

#include "boot/boot_anim.h"

#include "draw/draw_image.h"

#include "asset/asset_store.h"
#include "draw/draw_image_format.h"
#include "esp_log.h"
#include "gfx/gfx.h"

static const char* TAG = "boot_anim";
static handle_t load;

void
boot_anim_photo_load(image_t* out) {
    *out = (image_t){0};
    load = asset_store_load(BOOT_PHOTO);
    const asset_pack_t* pack = asset_store_loaded(load);
    if (pack == NULL) {
        asset_store_release(load);
        ESP_LOGW(TAG, "no pack %s: the animation draws without the photograph", BOOT_PHOTO);
        return;
    }
    image_t image = {0};
    asset_view_t entry;
    asset_status_t status = asset_pack_find(pack, BOOT_PHOTO, DRAW_IMAGE_ASSET, &entry);
    if (status == ASSET_OK) {
        status = draw_image_open(entry, &image);
    }
    if (status == ASSET_OK
        && (image.format != IMAGE_RGB565BE || image.width != GFX_WIDTH || image.height != GFX_HEIGHT)) {
        status = ASSET_ERR_FORMAT;
    }
    if (status != ASSET_OK) {
        ESP_LOGW(TAG, "picture %s: %s: the animation draws without the photograph", BOOT_PHOTO,
                 asset_status_text(status));
        asset_store_release(load);
        return;
    }
    *out = image;
}

void
boot_anim_photo_release(image_t* photo) {
    if (photo->pixels != NULL) {
        asset_store_release(load);
    }
    *photo = (image_t){0};
}
