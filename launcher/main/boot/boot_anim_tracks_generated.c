/*
 * GENERATED FILE - do not edit.
 *
 *     python tools/anim/bake_tracks.py main/boot/boot_anim_motion.glb --animation boot_motion --name boot_anim --out-dir main/boot
 *
 * Animation 'boot_motion' of the glTF, baked as authored.
 */

#include "boot_anim_tracks_generated.h"

static const float boot_anim_camera_translation_times[] = {0.0F};
static const float boot_anim_camera_translation_values[] = {0.0F, 0.0F, -10.0F};
const anim_track_t boot_anim_camera_translation = {boot_anim_camera_translation_times, boot_anim_camera_translation_values, 1, 3, ANIM_LINEAR, 0};

static const float boot_anim_camera_rotation_times[] = {0.0F};
static const float boot_anim_camera_rotation_values[] = {0.0F, 0.0F, 0.0F, 1.0F};
const anim_track_t boot_anim_camera_rotation = {boot_anim_camera_rotation_times, boot_anim_camera_rotation_values, 1, 4, ANIM_LINEAR, 1};

static const float boot_anim_camera_scale_times[] = {0.0F};
static const float boot_anim_camera_scale_values[] = {1.0F, 1.0F, 1.0F};
const anim_track_t boot_anim_camera_scale = {boot_anim_camera_scale_times, boot_anim_camera_scale_values, 1, 3, ANIM_LINEAR, 0};

static const float boot_anim_space_translation_times[] = {0.0F, 0.699999988F, 1.58000004F, 2.0F, 2.79999995F, 3.0999999F, 4.30000019F};
static const float boot_anim_space_translation_values[] = {0.0F, 0.0F, 0.0F, 10.0F, 0.0F, 0.0F, -42.8571434F, 0.0F, 0.0F, -0.0F, 0.0F, 0.0F, -5.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -5.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -5.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -5.0F, 0.0F, 0.0F, 10.0F, 26.666666F, 0.0F, 10.0F, 26.666666F, 0.0F, -2.0F, 8.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, 0.0F, -2.0F, 8.0F, 0.0F, 0.0F, 0.0F, 0.0F};
const anim_track_t boot_anim_space_translation = {boot_anim_space_translation_times, boot_anim_space_translation_values, 7, 3, ANIM_CUBIC, 0};

static const float boot_anim_space_rotation_times[] = {0.0F, 0.699999988F, 1.58000004F, 2.0F, 2.79999995F, 3.0999999F, 4.30000019F};
static const float boot_anim_space_rotation_values[] = {0.5F, 0.707106769F, -0.5F, 3.92523108e-17F, 0.5F, 0.707106769F, -0.5F, 3.92523108e-17F, 0.698401153F, 0.572061419F, -0.110615872F, 0.415626943F, 0.698401153F, 0.572061419F, -0.110615872F, 0.415626943F, 0.65328151F, 0.65328151F, -0.270598054F, 0.270598054F, 0.923879504F, 2.34326023e-17F, -0.382683426F, 5.65713056e-17F, 1.0F, 0.0F, 0.0F, 6.12323426e-17F};
const anim_track_t boot_anim_space_rotation = {boot_anim_space_rotation_times, boot_anim_space_rotation_values, 7, 4, ANIM_LINEAR, 1};

static const float boot_anim_space_scale_times[] = {0.0F, 0.699999988F, 1.58000004F, 2.0F, 2.79999995F, 3.0999999F, 4.30000019F};
static const float boot_anim_space_scale_values[] = {1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F, 0.600000024F, 0.600000024F, 0.600000024F};
const anim_track_t boot_anim_space_scale = {boot_anim_space_scale_times, boot_anim_space_scale_values, 7, 3, ANIM_LINEAR, 0};

const anim_track_t* const boot_anim_tracks[] = {
    &boot_anim_camera_translation,
    &boot_anim_camera_rotation,
    &boot_anim_camera_scale,
    &boot_anim_space_translation,
    &boot_anim_space_rotation,
    &boot_anim_space_scale,
};

const int boot_anim_track_count = 6;

const anim_clip_t boot_anim_clip = {4300};

const char* const boot_anim_track_names[] = {
    "camera/translation",
    "camera/rotation",
    "camera/scale",
    "space/translation",
    "space/rotation",
    "space/scale",
};
