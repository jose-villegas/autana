/*
 * Scene playback figure numbers from the library, emitted as JSON for playback_figures.py: a skeleton's clips
 * played through anim_layer_t frame by frame as a script says, with each slot's clip time, the blend weight,
 * and every joint's pose and position shown, sampled from the current clip alone and held in the previous slot.
 *
 *   playback_figures_probe PACK SKELETON SCRIPT
 *   playback_figures_probe --nlerp DEGREES STEPS
 *
 * SCRIPT, written by playback_figures.py: "clips N NAME...", "frame DT_MS END_MS", then events by time,
 * "MS play CLIP BLEND_MS", "MS wrap 0|1" (loop or clamp), "MS speed SLOT X" and "MS pause SLOT 0|1", SLOT 0 the current
 * and 1 the previous; each applies before that frame advances, as an app's calls do. --nlerp turns about one
 * axis by DEGREES in STEPS through quatf_nlerp() and quatf_slerp().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim/anim_skeleton.h"
#include "anim/anim_tracks.h"
#include "anim/playback/anim_layer.h"
#include "anim/pose/anim_pose.h"
#include "asset/asset_file.h"
#include "math/linear/mat4f.h"
#include "math/linear/quatf.h"
#include "math/scalar/math_const.h"

enum { CLIPS_MAX = 8, NAME_MAX_BYTES = 32, CURVES_MAX = 256, EVENTS_MAX = 64, WORD_MAX = 16 };

typedef struct {
    uint32_t at_ms;
    char what[WORD_MAX];
    float a, b;
} event_t;

static const vec3f_t TURN_AXIS = {0.0F, 0.0F, 1.0F};

static void
print_rotations(const anim_pose_t* pose) {
    printf("[");
    for (int j = 0; j < pose->joint_count; j++) {
        const quatf_t q = pose->local[j].rotation;
        printf("%s[%.7g,%.7g,%.7g,%.7g]", j ? "," : "", (double)q.x, (double)q.y, (double)q.z, (double)q.w);
    }
    printf("]");
}

/* Each joint's model-space origin; model is scratch of the skeleton's joint count. */
static void
print_joints(const anim_skeleton_t* skeleton, const anim_pose_t* pose, mat4f_t* model) {
    anim_pose_model(skeleton, pose, model);
    printf("[");
    for (int j = 0; j < pose->joint_count; j++) {
        const vec3f_t p = mat4f_apply(&model[j], (vec3f_t){0});
        printf("%s[%.7g,%.7g,%.7g]", j ? "," : "", (double)p.x, (double)p.y, (double)p.z);
    }
    printf("]");
}

static anim_playhead_t*
slot_playhead(anim_layer_t* layer, int slot) {
    return slot == 0 ? &layer->current.playhead : &layer->previous.playhead;
}

static void
apply(anim_layer_t* layer, const event_t* e, anim_wrap_t* wrap) {
    if (strcmp(e->what, "play") == 0) {
        anim_layer_play(layer, (uint8_t)e->a, (uint32_t)e->b);
        layer->current.playhead.wrap = *wrap; /* as scene_animator_play() sets the scene file's wrap */
    } else if (strcmp(e->what, "wrap") == 0) {
        *wrap = e->a != 0.0F ? ANIM_CLAMP : ANIM_LOOP;
        layer->current.playhead.wrap = *wrap;
    } else if (strcmp(e->what, "speed") == 0) {
        anim_playhead_set_speed(slot_playhead(layer, (int)e->a), e->b);
    } else if (strcmp(e->what, "pause") == 0) {
        slot_playhead(layer, (int)e->a)->paused = e->b != 0.0F;
    }
}

static int
nlerp(float degrees, int steps) {
    const quatf_t from = quatf_from_axis_angle(TURN_AXIS, 0.0F);
    const quatf_t to = quatf_from_axis_angle(TURN_AXIS, degrees * MATH_RADIANS_PER_DEGREE);
    printf("{\"degrees\":%.7g,\"steps\":[", (double)degrees);
    for (int s = 0; s <= steps; s++) {
        const float t = (float)s / (float)steps;
        const quatf_t n = quatf_nlerp(from, to, t), l = quatf_slerp(from, to, t);
        printf("%s{\"t\":%.7g,\"nlerp\":[%.7g,%.7g],\"slerp\":[%.7g,%.7g]}", s ? "," : "", (double)t, (double)n.z,
               (double)n.w, (double)l.z, (double)l.w);
    }
    printf("]}\n");
    return 0;
}

int
main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--nlerp") == 0) {
        return nlerp(strtof(argv[2], NULL), atoi(argv[3]));
    }
    asset_pack_t pack;
    void* buffer = NULL;
    FILE* in = argc == 4 ? fopen(argv[3], "r") : NULL;
    if (in == NULL || asset_file_open(argv[1], &pack, &buffer) != ASSET_OK) {
        fprintf(stderr, "usage: playback_figures_probe PACK SKELETON SCRIPT\n");
        return 2;
    }
    anim_skeleton_t skeleton;
    if (anim_skeleton_open(&pack, argv[2], &skeleton) != ASSET_OK) {
        fprintf(stderr, "playback_figures_probe: no skeleton %s\n", argv[2]);
        return 2;
    }
    int clip_count = 0;
    char names[CLIPS_MAX][NAME_MAX_BYTES];
    if (fscanf(in, " clips %d", &clip_count) != 1 || clip_count < 1 || clip_count > CLIPS_MAX) {
        return 2;
    }
    anim_tracks_t tracks[CLIPS_MAX];
    anim_clip_t clips[CLIPS_MAX];
    static anim_joint_bound_t bound[CLIPS_MAX][CURVES_MAX];
    const anim_joint_bound_t* tables[CLIPS_MAX];
    int counts[CLIPS_MAX];
    for (int c = 0; c < clip_count; c++) {
        int failed = -1;
        if (fscanf(in, " %31s", names[c]) != 1 || anim_tracks_from_pack(&pack, names[c], &tracks[c]) != ASSET_OK
            || tracks[c].count > CURVES_MAX
            || anim_bind_skeleton(&tracks[c], &skeleton, bound[c], tracks[c].count, &failed) != ANIM_BIND_OK) {
            fprintf(stderr, "playback_figures_probe: clip %d does not bind\n", c);
            return 2;
        }
        clips[c] = tracks[c].clip;
        tables[c] = bound[c];
        counts[c] = tracks[c].count;
    }
    unsigned dt_ms = 0, end_ms = 0;
    event_t events[EVENTS_MAX];
    int event_count = 0;
    if (fscanf(in, " frame %u %u", &dt_ms, &end_ms) != 2 || dt_ms == 0) {
        return 2;
    }
    while (event_count < EVENTS_MAX) {
        event_t* e = &events[event_count];
        if (fscanf(in, " %u %15s %f", &e->at_ms, e->what, &e->a) != 3) {
            break;
        }
        if (strcmp(e->what, "play") == 0 || strcmp(e->what, "speed") == 0 || strcmp(e->what, "pause") == 0) {
            if (fscanf(in, " %f", &e->b) != 1) {
                return 2;
            }
        }
        event_count++;
    }
    fclose(in);

    const size_t joints = skeleton.joint_count;
    transformf_t* storage = calloc(3 * joints, sizeof *storage);
    mat4f_t* model = calloc(joints, sizeof *model);
    if (storage == NULL || model == NULL) {
        return 1;
    }
    const anim_pose_t shown = {storage, skeleton.joint_count}, frozen = {storage + joints, skeleton.joint_count};
    anim_pose_t alone = {storage + (2 * joints), skeleton.joint_count};
    anim_layer_t layer;
    anim_layer_init(&layer, clips, clip_count, shown, frozen);

    printf("{\"clips\":[");
    for (int c = 0; c < clip_count; c++) {
        printf("%s{\"name\":\"%s\",\"duration_ms\":%u}", c ? "," : "", names[c], (unsigned)clips[c].duration_ms);
    }
    printf("],\"parents\":[");
    for (size_t j = 0; j < joints; j++) {
        printf("%s%d", j ? "," : "", skeleton.joints[j].parent == ANIM_SKELETON_ROOT ? -1 : skeleton.joints[j].parent);
    }
    printf("],\"paths\":[");
    for (size_t j = 0; j < joints; j++) {
        printf("%s\"%s\"", j ? "," : "", skeleton.paths + skeleton.joints[j].path);
    }
    printf("],\"frames\":[");
    anim_wrap_t wrap = ANIM_LOOP;
    int next = 0, printed = 0;
    for (uint32_t ms = 0; ms <= end_ms; ms += dt_ms) {
        /* Calls at or before this frame apply first, then the frame advances by its dt, as a frame loop runs. */
        while (next < event_count && events[next].at_ms <= ms) {
            apply(&layer, &events[next++], &wrap);
        }
        if (ms != 0) {
            anim_layer_advance(&layer, dt_ms);
        }
        if (layer.current.clip == ANIM_LAYER_CLIP_NONE) {
            continue;
        }
        const bool blending = layer.frozen_previous || layer.previous.clip != ANIM_LAYER_CLIP_NONE;
        const anim_pose_t* pose = anim_layer_pose(&layer, &skeleton, tables, counts);
        printf("%s{\"ms\":%u,\"weight\":%.7g,\"frozen\":%s,\"current\":[%d,%u],\"previous\":", printed++ ? "," : "",
               (unsigned)ms, (double)anim_layer_weight(&layer), layer.frozen_previous ? "true" : "false",
               layer.current.clip, (unsigned)layer.current.playhead.t_ms);
        if (layer.previous.clip != ANIM_LAYER_CLIP_NONE) {
            printf("[%d,%u]", layer.previous.clip, (unsigned)layer.previous.playhead.t_ms);
        } else {
            printf("null");
        }
        printf(",\"shown\":");
        print_rotations(pose);
        printf(",\"joints\":");
        print_joints(&skeleton, pose, model);
        anim_pose_rest(&skeleton, &alone);
        anim_sample_pose(tables[layer.current.clip], counts[layer.current.clip],
                         anim_playhead_seconds(&layer.current.playhead), &alone);
        printf(",\"current_alone\":");
        print_rotations(&alone);
        printf(",\"current_joints\":");
        print_joints(&skeleton, &alone, model);
        if (blending) {
            printf(",\"previous_pose\":");
            print_rotations(&layer.frozen);
            printf(",\"previous_joints\":");
            print_joints(&skeleton, &layer.frozen, model);
        } else {
            printf(",\"previous_pose\":null,\"previous_joints\":null");
        }
        printf("}");
    }
    printf("]}\n");
    free(model);
    free(storage);
    asset_file_release(buffer);
    return 0;
}
