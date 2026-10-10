/*
 * track_host: prints what a clip's tracks hold every N milliseconds, read
 * from the clip's TRCK entry in an asset pack through the same
 * anim_tracks_from_pack(), anim_clip_seconds() and anim_track_sample() the
 * firmware calls. One program for every clip: track_host.py builds it once.
 *
 *   track_host --pack PACK --clip ID [--from MS] [--every MS] [--until MS] [--clamp]
 *   track_host --pack SCRATCH --clip ID [--every MS] [--until MS] --source-poses NODE W H TAN NEAR
 *
 * By default one line per track per sample time: `<t_ms> <name> <value...>`,
 * from --from (0) to --until (the clip's duration). With --source-poses it prints the
 * poses file r3d's triangle_sizes reads, for the camera node NODE: its
 * translation as the eye, its rotation turning -z, the source frame's
 * camera forward; --source-poses is only for the sampler's source-space
 * scratch pack. These bake-time artifacts use the lens and
 * size given, from 0 and looping, so --from and --clamp are refused with it. --source-poses comes last. --every 0 is refused.
 * The clock counts in 64 bits, so a step past an --until near the u32
 * maximum ends the run instead of wrapping to 0.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim/anim_track.h"
#include "anim/anim_tracks.h"
#include "asset/asset_file.h"

typedef struct {
    uint32_t from_ms;
    uint32_t every_ms;
    uint32_t until_ms;
    bool until_given;
    anim_wrap_t wrap;
} sampling_t;

static int
usage(void) {
    fprintf(stderr,
            "usage: track_host --pack PACK --clip ID [--from MS] [--every MS] [--until MS] [--clamp]\n"
            "       track_host --pack PACK --clip ID [--every MS] [--until MS] --source-poses NODE W H TAN NEAR\n");
    return 2;
}

static int
print_poses(const anim_tracks_t* tracks, char** pose_args, const sampling_t* at) {
    const char* node = pose_args[0];
    anim_node_tracks_t path;
    const asset_status_t found = anim_tracks_find_node(tracks, node, &path);
    if (found != ASSET_OK) {
        fprintf(stderr, "track_host: node %s: %s\n", node, asset_status_text(found));
        return 2;
    }
    printf("size %d %d\nlens %s %s\n", atoi(pose_args[1]), atoi(pose_args[2]), pose_args[3], pose_args[4]);
    const float ahead[3] = {0.0F, 0.0F, -1.0F};
    for (uint64_t t = 0; t < at->until_ms; t += at->every_ms) {
        float eye[ANIM_WIDTH_MAX];
        float q[ANIM_WIDTH_MAX];
        float forward[3];
        const float seconds = anim_clip_seconds(&tracks->clip, (uint32_t)t, ANIM_LOOP);
        anim_track_sample(&path.translation, seconds, eye);
        anim_track_sample(&path.rotation, seconds, q);
        anim_quat_rotate(q, ahead, forward);
        printf("pose %.9g %.9g %.9g %.9g %.9g %.9g\n", (double)eye[0], (double)eye[1], (double)eye[2],
               (double)forward[0], (double)forward[1], (double)forward[2]);
    }
    return 0;
}

static int
print_tracks(const anim_tracks_t* tracks, const sampling_t* at) {
    for (uint64_t t = at->from_ms; t < at->until_ms; t += at->every_ms) {
        const float seconds = anim_clip_seconds(&tracks->clip, (uint32_t)t, at->wrap);
        for (int i = 0; i < tracks->count; i++) {
            const char* name;
            anim_track_t track;
            (void)anim_tracks_at(tracks, i, &name, &track);
            float v[ANIM_WIDTH_MAX];
            anim_track_sample(&track, seconds, v);
            printf("%" PRIu64 " %s", t, name);
            for (int k = 0; k < track.width; k++) {
                printf(" %.9g", (double)v[k]);
            }
            printf("\n");
        }
    }
    return 0;
}

static int
sample(const char* pack_path, const char* clip, char** pose_args, sampling_t* at) {
    asset_pack_t pack;
    void* buffer;
    asset_status_t status = asset_file_open(pack_path, &pack, &buffer);
    if (status != ASSET_OK) {
        fprintf(stderr, "track_host: pack %s: %s\n", pack_path, asset_status_text(status));
        return 2;
    }
    anim_tracks_t tracks;
    status = anim_tracks_from_pack(&pack, clip, &tracks);
    int result = 2;
    if (status != ASSET_OK) {
        fprintf(stderr, "track_host: clip %s in %s: %s\n", clip, pack_path, asset_status_text(status));
    } else {
        if (!at->until_given) {
            at->until_ms = tracks.clip.duration_ms;
        }
        result = pose_args != NULL ? print_poses(&tracks, pose_args, at) : print_tracks(&tracks, at);
    }
    asset_file_release(buffer);
    return result;
}

int
main(int argc, char** argv) {
    const char* pack_path = NULL;
    const char* clip = NULL;
    char** pose_args = NULL;
    sampling_t at = {.every_ms = 5000, .wrap = ANIM_LOOP};
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pack") == 0 && i + 1 < argc) {
            pack_path = argv[++i];
        } else if (strcmp(argv[i], "--clip") == 0 && i + 1 < argc) {
            clip = argv[++i];
        } else if (strcmp(argv[i], "--from") == 0 && i + 1 < argc) {
            at.from_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--every") == 0 && i + 1 < argc) {
            at.every_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--until") == 0 && i + 1 < argc) {
            at.until_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
            at.until_given = true;
        } else if (strcmp(argv[i], "--clamp") == 0) {
            at.wrap = ANIM_CLAMP;
        } else if (strcmp(argv[i], "--poses") == 0) {
            fprintf(stderr, "track_host: --poses requires track_host.py and a source-space .anim.toml, not --pack\n");
            return usage();
        } else if (strcmp(argv[i], "--source-poses") == 0 && i + 6 == argc) {
            pose_args = argv + i + 1;
            break;
        } else {
            return usage();
        }
    }
    if (pack_path == NULL || clip == NULL || at.every_ms == 0
        || (pose_args != NULL && (at.from_ms != 0 || at.wrap == ANIM_CLAMP))) {
        return usage();
    }
    return sample(pack_path, clip, pose_args, &at);
}
