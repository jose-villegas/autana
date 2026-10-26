/*
 * Scene playback figure numbers from the library, emitted as JSON for playback_figures.py: a skeleton's clips
 * played on a scene entity through scene_animator_play(), scene_animator_set_speed() and
 * scene_animator_set_paused() and stepped by scene_animation_step() frame by frame as a script says, with each
 * slot's clip time, the blend weight, and every joint's pose and position shown, sampled from the current clip
 * alone and held in the previous slot.
 *
 *   playback_figures_probe PACK SCENE ENTITY SKELETON SCRIPT
 *   playback_figures_probe --nlerp DEGREES STEPS
 *
 * SCRIPT, written by playback_figures.py: "clips N NAME...", "wrap 0|1" (loop or clamp), "frame DT_MS END_MS",
 * then calls in time order, "MS play CLIP BLEND_MS", "MS speed SLOT X" and "MS pause SLOT 0|1", SLOT a
 * scene_animator_slot_t; each runs before that frame steps, and a call the scene refuses fails the run. --nlerp
 * turns about one axis by DEGREES in STEPS through quatf_nlerp() and quatf_slerp().
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_file.h"
#include "math/linear/quatf.h"
#include "math/scalar/math_const.h"
#include "math/text/math_text.h"
#include "scene/scene_internal.h"

#define STRINGIFY(x) #x
#define WIDTH(x)     STRINGIFY(x)
#define NAME_CHARS   31 /* ASSET_NAME_MAX less the NUL, as a scanf width */
#define CALL_CHARS   7
_Static_assert(NAME_CHARS + 1 == ASSET_NAME_MAX, "scanf width follows ASSET_NAME_MAX");

static const vec3f_t TURN_AXIS = {0.0F, 0.0F, 1.0F};

static int
fail(const char* why) {
    fprintf(stderr, "playback_figures_probe: %s\n", why);
    return 2;
}

static void
print_rotations(const anim_pose_t* pose) {
    printf("[");
    for (int j = 0; j < pose->joint_count; j++) {
        const quatf_t q = pose->local[j].rotation;
        char text[QUATF_TEXT_MAX];
        quatf_format(text, sizeof text, q);
        printf("%s[%s]", j ? "," : "", text);
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
        char text[VEC3F_TEXT_MAX];
        vec3f_format(text, sizeof text, p);
        printf("%s[%s]", j ? "," : "", text);
    }
    printf("]");
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

/* Runs the script's next call; false when the scene refuses it or the line does not parse. */
static bool
call(FILE* in, scene_t* scene, scene_entity_t entity) {
    char what[CALL_CHARS + 1];
    float a = 0.0F, b = 0.0F;
    if (fscanf(in, " %" WIDTH(CALL_CHARS) "s %f %f", what, &a, &b) != 3) {
        return false;
    }
    if (strcmp(what, "play") == 0) {
        return scene_animator_play(scene, entity, (int)a, (uint32_t)b);
    }
    if (strcmp(what, "speed") == 0) {
        return scene_animator_set_speed(scene, entity, (scene_animator_slot_t)a, b);
    }
    return strcmp(what, "pause") == 0 && scene_animator_set_paused(scene, entity, (scene_animator_slot_t)a, b != 0.0F);
}

static void
print_frame(const scene_animator_t* a, uint32_t ms, bool first, anim_pose_t* alone, mat4f_t* model) {
    const anim_layer_t* layer = &a->layer;
    const bool blending = layer->frozen_previous || layer->previous.clip != ANIM_LAYER_CLIP_NONE;
    printf("%s{\"ms\":%u,\"weight\":%.7g,\"frozen\":%s,\"current\":[%d,%u],\"previous\":", first ? "" : ",",
           (unsigned)ms, (double)anim_layer_weight(layer), layer->frozen_previous ? "true" : "false",
           layer->current.clip, (unsigned)layer->current.playhead.t_ms);
    if (layer->previous.clip != ANIM_LAYER_CLIP_NONE) {
        printf("[%d,%u]", layer->previous.clip, (unsigned)layer->previous.playhead.t_ms);
    } else {
        printf("null");
    }
    printf(",\"shown\":");
    print_rotations(&layer->shown);
    printf(",\"joints\":");
    print_joints(&a->skeleton, &layer->shown, model);
    anim_pose_rest(&a->skeleton, alone);
    anim_sample_pose(a->joint_tables[layer->current.clip], a->counts[layer->current.clip],
                     anim_playhead_seconds(&layer->current.playhead), alone);
    printf(",\"current_alone\":");
    print_rotations(alone);
    printf(",\"current_joints\":");
    print_joints(&a->skeleton, alone, model);
    if (blending) {
        printf(",\"previous_pose\":");
        print_rotations(&layer->frozen);
        printf(",\"previous_joints\":");
        print_joints(&a->skeleton, &layer->frozen, model);
    } else {
        printf(",\"previous_pose\":null,\"previous_joints\":null");
    }
    printf("}");
}

static void
print_rig(const scene_t* scene, scene_entity_t entity, char (*names)[ASSET_NAME_MAX], int count) {
    const anim_skeleton_t* skeleton = &scene->animators[entity].skeleton;
    printf("{\"clips\":[");
    for (int c = 0; c < count; c++) {
        printf("%s{\"name\":\"%s\",\"duration_ms\":%u}", c ? "," : "", names[c],
               (unsigned)scene_animator_clip_duration_ms(scene, entity, c));
    }
    printf("],\"parents\":[");
    for (int j = 0; j < skeleton->joint_count; j++) {
        const uint8_t parent = skeleton->joints[j].parent;
        printf("%s%d", j ? "," : "", parent == ANIM_SKELETON_ROOT ? -1 : parent);
    }
    printf("],\"paths\":[");
    for (int j = 0; j < skeleton->joint_count; j++) {
        printf("%s\"%s\"", j ? "," : "", skeleton->paths + skeleton->joints[j].path);
    }
    printf("],\"frames\":[");
}

static int
play(FILE* in, scene_t* scene, scene_entity_t entity, const char* skeleton) {
    int count = 0, wrap = 0;
    if (fscanf(in, " clips %d", &count) != 1 || count < 1 || count > ANIM_LAYER_CLIP_NONE) {
        return fail("script: no clips line");
    }
    char (*names)[ASSET_NAME_MAX] = calloc((size_t)count, sizeof *names);
    const char** ids = calloc((size_t)count, sizeof *ids);
    if (names == NULL || ids == NULL) {
        return fail("out of memory");
    }
    for (int c = 0; c < count; c++) {
        if (fscanf(in, " %" WIDTH(NAME_CHARS) "s", names[c]) != 1) {
            return fail("script: fewer clip names than clips");
        }
        ids[c] = names[c];
    }
    scene_failure_t why;
    if (!scene_animator_set_clips(scene, entity, skeleton, ids, count, &why)) {
        fprintf(stderr, "playback_figures_probe: %s does not bind: %s\n", why.what, asset_status_text(why.asset));
        return 2;
    }
    unsigned dt_ms = 0, end_ms = 0;
    if (fscanf(in, " wrap %d frame %u %u", &wrap, &dt_ms, &end_ms) != 3 || dt_ms == 0) {
        return fail("script: no wrap and frame lines");
    }
    scene_animator_t* a = &scene->animators[entity];
    a->wrap = wrap ? ANIM_CLAMP : ANIM_LOOP; /* what scene.c's open_animator() sets from a scene file's wrap */
    anim_pose_t alone = {calloc(a->skeleton.joint_count, sizeof(transformf_t)), a->skeleton.joint_count};
    mat4f_t* model = calloc(a->skeleton.joint_count, sizeof *model);
    if (alone.local == NULL || model == NULL) {
        return fail("out of memory");
    }
    print_rig(scene, entity, names, count);
    unsigned at = 0;
    bool pending = fscanf(in, " %u", &at) == 1, first = true;
    for (uint32_t ms = 0; ms <= end_ms; ms += dt_ms) {
        for (; pending && at <= ms; pending = fscanf(in, " %u", &at) == 1) {
            if (!call(in, scene, entity)) {
                fprintf(stderr, "playback_figures_probe: the call at %u ms failed\n", at);
                return 2;
            }
        }
        scene_animation_step(scene, ms == 0 ? 0 : dt_ms);
        if (a->layer.current.clip != ANIM_LAYER_CLIP_NONE) {
            print_frame(a, ms, first, &alone, model);
            first = false;
        }
    }
    printf("]}\n");
    if (pending || !feof(in)) {
        return fail("script: calls after the last frame, or a line that does not parse");
    }
    free(model);
    free(alone.local);
    free(ids);
    free(names);
    return 0;
}

int
main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "--nlerp") == 0) {
        return nlerp(strtof(argv[2], NULL), atoi(argv[3]));
    }
    if (argc != 6) {
        return fail("usage: playback_figures_probe PACK SCENE ENTITY SKELETON SCRIPT");
    }
    asset_pack_t pack;
    void* buffer = NULL;
    FILE* in = fopen(argv[5], "r");
    if (in == NULL || asset_file_open(argv[1], &pack, &buffer) != ASSET_OK) {
        return fail("cannot open the pack or the script");
    }
    scene_failure_t why;
    scene_t* scene = scene_load_from(&pack, argv[2], &why);
    if (scene == NULL) {
        return fail("the scene does not load");
    }
    const int status = play(in, scene, scene_find(scene, name_hash(argv[3])), argv[4]);
    fclose(in);
    scene_unload(scene);
    asset_file_release(buffer);
    return status;
}
