/*
 * sample_tracks: prints what a baked animation's tracks hold every N
 * milliseconds, through the same anim_clip_seconds() and anim_track_sample()
 * the firmware calls. Built by sample_tracks.sh against one baked animation,
 * whose clip ANIM_CLIP and track names ANIM_NAMES it is compiled with.
 *
 *   sample_tracks [--from MS] [--every MS] [--until MS] [--clamp] [--poses NODE W H TAN NEAR]
 *
 * By default a line a track per time: `<t_ms> <name> <value...>`, from
 * --from (0) to --until (the clip's duration). With --poses it prints the
 * poses file r3d's triangle_sizes reads, for the camera node NODE: its
 * translation as the eye, the way its rotation turns glTF's -Z as the
 * forward, over the lens and size given. --poses comes last.
 */
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim/anim_track.h"

#define POSES_MAX 64

extern const anim_clip_t ANIM_CLIP;
extern const char* const ANIM_NAMES[];

static const anim_track_t*
find_track(const char* node, const char* path) {
    char name[128];
    snprintf(name, sizeof name, "%s/%s", node, path);
    for (int i = 0; i < ANIM_CLIP.count; i++) {
        if (strcmp(ANIM_NAMES[i], name) == 0) {
            return ANIM_CLIP.tracks[i];
        }
    }
    fprintf(stderr, "sample_tracks: no track %s\n", name);
    exit(2);
}

static int
print_poses(const char* node, int width, int height, const char* tan, const char* near_z, uint32_t every_ms,
            uint32_t until_ms) {
    const anim_track_t* move = find_track(node, "translation");
    const anim_track_t* turn = find_track(node, "rotation");
    if (every_ms == 0 || until_ms / every_ms >= POSES_MAX) {
        fprintf(stderr, "sample_tracks: more than %d poses; raise --every\n", POSES_MAX);
        return 2;
    }
    printf("size %d %d\nlens %s %s\n", width, height, tan, near_z);
    const float ahead[3] = {0.0F, 0.0F, -1.0F};
    for (uint32_t t = 0; t < until_ms; t += every_ms) {
        float eye[ANIM_WIDTH_MAX];
        float q[ANIM_WIDTH_MAX];
        float forward[3];
        const float seconds = anim_clip_seconds(&ANIM_CLIP, t, ANIM_LOOP);
        anim_track_sample(move, seconds, eye);
        anim_track_sample(turn, seconds, q);
        anim_quat_rotate(q, ahead, forward);
        printf("pose %.9g %.9g %.9g %.9g %.9g %.9g\n", (double)eye[0], (double)eye[1], (double)eye[2],
               (double)forward[0], (double)forward[1], (double)forward[2]);
    }
    return 0;
}

int
main(int argc, char** argv) {
    uint32_t from_ms = 0;
    uint32_t every_ms = 5000;
    int until_given = 0;
    uint32_t until_ms = ANIM_CLIP.duration_ms;
    anim_wrap_t wrap = ANIM_LOOP;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            from_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) {
            every_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) {
            until_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
            until_given = 1;
        } else if (strcmp(argv[i], "--clamp") == 0) {
            wrap = ANIM_CLAMP;
        } else if (strcmp(argv[i], "--poses") == 0 && i + 5 < argc) {
            return print_poses(argv[i + 1], atoi(argv[i + 2]), atoi(argv[i + 3]), argv[i + 4], argv[i + 5], every_ms,
                               until_ms);
        } else {
            fprintf(
                stderr,
                "usage: sample_tracks [--from MS] [--every MS] [--until MS] [--clamp] [--poses NODE W H TAN NEAR]\n");
            return 2;
        }
    }
    if (every_ms == 0) {
        return 2;
    }
    if (!until_given && from_ms > until_ms) {
        until_ms = from_ms;
    }
    for (uint32_t t = from_ms; t < until_ms; t += every_ms) {
        const float seconds = anim_clip_seconds(&ANIM_CLIP, t, wrap);
        for (int i = 0; i < ANIM_CLIP.count; i++) {
            float v[ANIM_WIDTH_MAX];
            anim_track_sample(ANIM_CLIP.tracks[i], seconds, v);
            printf("%" PRIu32 " %s", t, ANIM_NAMES[i]);
            for (int k = 0; k < ANIM_CLIP.tracks[i]->width; k++) {
                printf(" %.9g", (double)v[k]);
            }
            printf("\n");
        }
    }
    return 0;
}
