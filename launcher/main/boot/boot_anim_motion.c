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

typedef struct {
    const char* name;
    uint8_t width;
    uint8_t quaternion;
} binding_t;

static const binding_t bindings[BOOT_ANIM_TRACKS] = {
    [BOOT_ANIM_CAMERA_TRANSLATION] = {"camera/translation", 3, 0},
    [BOOT_ANIM_CAMERA_ROTATION] = {"camera/rotation", 4, 1},
    [BOOT_ANIM_CAMERA_SCALE] = {"camera/scale", 3, 0},
    [BOOT_ANIM_SPACE_TRANSLATION] = {"space/translation", 3, 0},
    [BOOT_ANIM_SPACE_ROTATION] = {"space/rotation", 4, 1},
    [BOOT_ANIM_SPACE_SCALE] = {"space/scale", 3, 0},
};

/* The camera 10 m back on -z, unturned. The space turned a quarter about the
 * view axis, so t climbs the way the panel is read, then tipped 45 degrees
 * back about x so the floor reads as a plane; shrunk and moved down the
 * reader's view so the whole climb fits beside the title. */
static const float REST_TIME[] = {0.0F};
static const float REST_CAMERA_AT[] = {0.0F, 0.0F, -10.0F};
static const float REST_UNTURNED[] = {0.0F, 0.0F, 0.0F, 1.0F};
static const float REST_UNSCALED[] = {1.0F, 1.0F, 1.0F};
static const float REST_SPACE_AT[] = {-4.5F, 0.0F, 0.0F};
static const float REST_SPACE_TURN[] = {-0.270598F, 0.270598F, -0.653281F, 0.653281F};
static const float REST_SPACE_SIZE[] = {0.42F, 0.42F, 0.42F};

#define REST_TRACK(values, width, quaternion) {REST_TIME, values, 1, width, ANIM_STEP, quaternion}

static void
rest(boot_anim_motion_t* out) {
    *out = (boot_anim_motion_t){
        .clip = {0},
        .t =
            {
                [BOOT_ANIM_CAMERA_TRANSLATION] = REST_TRACK(REST_CAMERA_AT, 3, 0),
                [BOOT_ANIM_CAMERA_ROTATION] = REST_TRACK(REST_UNTURNED, 4, 1),
                [BOOT_ANIM_CAMERA_SCALE] = REST_TRACK(REST_UNSCALED, 3, 0),
                [BOOT_ANIM_SPACE_TRANSLATION] = REST_TRACK(REST_SPACE_AT, 3, 0),
                [BOOT_ANIM_SPACE_ROTATION] = REST_TRACK(REST_SPACE_TURN, 4, 1),
                [BOOT_ANIM_SPACE_SCALE] = REST_TRACK(REST_SPACE_SIZE, 3, 0),
            },
        .from_pack = false,
    };
}

/* Each track boot binds, found and shaped as boot samples it: a rotation one
 * narrower than four would leave a quaternion component unwritten. */
static asset_status_t
bind(const anim_tracks_t* tracks, boot_anim_motion_t* out, int* failed) {
    for (int i = 0; i < BOOT_ANIM_TRACKS; i++) {
        *failed = i;
        const asset_status_t found = anim_tracks_find(tracks, bindings[i].name, &out->t[i]);
        if (found != ASSET_OK) {
            return found;
        }
        if (out->t[i].width != bindings[i].width || (out->t[i].quaternion != 0) != (bindings[i].quaternion != 0)) {
            return ASSET_ERR_FORMAT;
        }
    }
    out->clip = tracks->clip;
    return ASSET_OK;
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
    int failed = -1;
    asset_status_t status = anim_tracks_from_pack(bundle, BOOT_CLIP, &tracks);
    if (status == ASSET_OK) {
        status = bind(&tracks, out, &failed);
    }
    if (status != ASSET_OK) {
        ESP_LOGW(TAG, "clip %s%s%s: %s: camera and space hold the rest pose", BOOT_CLIP, failed < 0 ? "" : " track ",
                 failed < 0 ? "" : bindings[failed].name, asset_status_text(status));
        asset_store_release(BOOT_CLIP);
        rest(out);
        return;
    }
    out->from_pack = true;
}

void
boot_anim_motion_release(boot_anim_motion_t* motion) {
    if (motion->from_pack) {
        asset_store_release(BOOT_CLIP);
    }
    rest(motion);
}
