/*
 * boot_anim_motion: the camera and space the startup animation draws through,
 * read from the boot clip's own bundle, one TRCK entry baked from
 * boot_anim_motion.anim.toml.
 *
 * When the clip cannot be read (no assets partition, a bad directory or
 * bundle, a missing or malformed clip) the camera and space hold a rest pose
 * instead, so boot is never blank. The rest pose is authored here to frame
 * the whole climb, and is deliberately not a copy of a key of the clip: that
 * would be a second representation of the .glb, free to drift from it.
 */

#include "boot/boot_anim.h"

#include "anim/anim_tracks.h"
#include "asset/asset_store.h"
#include "esp_log.h"

#define BOOT_CLIP "boot_anim_motion"

static const char* TAG = "boot_anim";

/* The camera back on -z, unturned. The space turned a quarter about the view
 * axis, so t climbs the way the panel is read, and tipped 25 degrees toward
 * the camera so the floor reads as a plane; at its size the whole climb stays
 * on the panel, short of the camera. */
static const float REST_TIME[] = {0.0F};
static const float REST_CAMERA_AT[] = {0.0F, 0.0F, -14.0F};
static const float REST_UNTURNED[] = {0.0F, 0.0F, 0.0F, 1.0F};
static const float REST_UNSCALED[] = {1.0F, 1.0F, 1.0F};
static const float REST_SPACE_AT[] = {-9.0F, 0.0F, 0.0F};
static const float REST_SPACE_TURN[] = {-0.153046F, 0.153046F, -0.690346F, 0.690346F};
static const float REST_SPACE_SIZE[] = {0.5F, 0.5F, 0.5F};

#define REST_TRACK(values, width) {REST_TIME, values, 1, width, ANIM_STEP, (width) == 4}

static void
rest(boot_anim_motion_t* out) {
    *out = (boot_anim_motion_t){
        .clip = {0},
        .camera = {REST_TRACK(REST_CAMERA_AT, 3), REST_TRACK(REST_UNTURNED, 4), REST_TRACK(REST_UNSCALED, 3)},
        .space = {REST_TRACK(REST_SPACE_AT, 3), REST_TRACK(REST_SPACE_TURN, 4), REST_TRACK(REST_SPACE_SIZE, 3)},
        .from_pack = false,
    };
}

void
boot_anim_motion_load(boot_anim_motion_t* out) {
    const asset_pack_t* bundle = asset_store_bundle(BOOT_CLIP);
    if (bundle == NULL) {
        ESP_LOGW(TAG, "no bundle %s: camera and space hold the rest pose", BOOT_CLIP);
        rest(out);
        return;
    }
    anim_tracks_t tracks;
    const char* node = NULL;
    asset_status_t status = anim_tracks_from_pack(bundle, BOOT_CLIP, &tracks);
    if (status == ASSET_OK) {
        node = "camera";
        status = anim_tracks_find_node(&tracks, node, &out->camera);
    }
    if (status == ASSET_OK) {
        node = "space";
        status = anim_tracks_find_node(&tracks, node, &out->space);
    }
    if (status != ASSET_OK) {
        ESP_LOGW(TAG, "clip %s%s%s: %s: camera and space hold the rest pose", BOOT_CLIP, node == NULL ? "" : " node ",
                 node == NULL ? "" : node, asset_status_text(status));
        asset_store_release(BOOT_CLIP);
        rest(out);
        return;
    }
    out->clip = tracks.clip;
    out->from_pack = true;
}

void
boot_anim_motion_release(boot_anim_motion_t* motion) {
    if (motion->from_pack) {
        asset_store_release(BOOT_CLIP);
    }
    rest(motion);
}
