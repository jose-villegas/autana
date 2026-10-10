/*
 * boot_anim_photo: the photograph the startup animation crossfades to, read
 * from the boot picture's own pack, one IMAG entry baked from boot.png.
 *
 * It is drawn as one full-panel copy, so a picture of any other size or
 * format is refused like a missing one: the animation then draws on without it.
 */

#include "boot/boot_anim.h"

#include "asset/asset_store.h"
#include "esp_log.h"
#include "gfx/gfx.h"

#define BOOT_PHOTO "boot"

static const char* TAG = "boot_anim";

void
boot_anim_photo_load(boot_anim_photo_t* out) {
    *out = (boot_anim_photo_t){0};
    const asset_pack_t* pack = asset_store_pack(BOOT_PHOTO);
    if (pack == NULL) {
        ESP_LOGW(TAG, "no pack %s: the animation draws without the photograph", BOOT_PHOTO);
        return;
    }
    gfx_image_t image;
    asset_status_t status = gfx_image_from_pack(pack, BOOT_PHOTO, &image);
    if (status == ASSET_OK
        && (image.format != GFX_IMAGE_RGB565 || image.width != GFX_WIDTH || image.height != GFX_HEIGHT)) {
        status = ASSET_ERR_FORMAT;
    }
    if (status != ASSET_OK) {
        ESP_LOGW(TAG, "picture %s: %s: the animation draws without the photograph", BOOT_PHOTO,
                 asset_status_text(status));
        asset_store_release(BOOT_PHOTO);
        return;
    }
    *out = (boot_anim_photo_t){.image = image, .from_pack = true};
}

void
boot_anim_photo_release(boot_anim_photo_t* photo) {
    if (photo->from_pack) {
        asset_store_release(BOOT_PHOTO);
    }
    *photo = (boot_anim_photo_t){0};
}
