/* Camera field: resolve a lens curve and sample its field of view. */
#include "anim/anim_tracks.h"

static asset_status_t
camera_field_example_start(const anim_tracks_t* clip, anim_track_t* lens) {
    const name_hash_t camera = name_hash("camera");
    const name_hash_t field = name_hash("half_fov_short_tan");
    return anim_tracks_find(clip, camera, ANIM_COMPONENT_CAMERA, field, lens);
}

static void
camera_field_example(const anim_track_t* lens, float seconds, float fov[ANIM_WIDTH_MAX]) {
    anim_track_sample(lens, seconds, fov);
}
