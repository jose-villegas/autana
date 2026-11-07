/*
 * r3d_lod: which level of a mesh's clusters an instance draws
 * (docs/plans/Cluster-LOD.md). Once per instance a frame, r3d_lod_pick()
 * sets one bit per group: coarse when the group's error, projected at its
 * bounds' nearest point, is within the tolerance and its bounds' radius,
 * projected there, spans at most a share of the picture's height. The
 * cluster walk then keeps a cluster when r3d_lod_drawn() says so, before its
 * box test.
 *
 * A coarse group's bit only ever stays or clears within a band of
 * R3D_LOD_HYSTERESIS_PX around the tolerance, so a camera holding still at
 * a switch does not flicker. The bits persist from frame to frame with the
 * instance; all clear is a valid start, every cluster finest.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_lit_mesh.h"
#include "render/r3d_pipeline.h"

/* The projected error, in pixels, a group may have and still be drawn
 * coarse; fixed until a frame budget sets it. */
#define R3D_LOD_TOLERANCE_PX      2.0F
/* A coarse group stays coarse until its error passes the tolerance by this
 * much, and a fine one turns coarse once it is this much under. */
#define R3D_LOD_HYSTERESIS_PX     0.5F

/* The thousandths of the picture's height a group's projected radius may
 * span and still be drawn coarse; R3D_LOD_UNCAPPED lifts the cap. A large
 * coarse group shows its simplified surface over much of the screen, where a
 * slow camera drift reads its point-sampled colours as popping
 * (docs/plans/Cluster-LOD.md, the coverage cap). */
#define R3D_LOD_COVERAGE_PERMILLE 250
#define R3D_LOD_PERMILLE          1000
#define R3D_LOD_UNCAPPED          INT32_MAX
/* The most frames one round of the pick may be spread over (an experiment,
 * render.lod_pick_every): each frame picks every Nth instance, so a level
 * change lands up to N - 1 frames late. */
#define R3D_LOD_PICK_EVERY_MAX    8

#define R3D_LOD_WORD_BITS         32
/* The words of coarse bits a mesh's groups need. */
#define R3D_LOD_WORDS(groups)     (((groups) + R3D_LOD_WORD_BITS - 1) / R3D_LOD_WORD_BITS)

static inline bool
r3d_lod_coarse(const uint32_t* coarse, uint16_t group) {
    return ((coarse[group / R3D_LOD_WORD_BITS] >> (group % R3D_LOD_WORD_BITS)) & 1U) != 0;
}

/* Whether any of a mesh's `groups` is coarse in `coarse`. */
static inline bool
r3d_lod_any(const uint32_t* coarse, int groups) {
    for (int w = 0; w < R3D_LOD_WORDS(groups); w++) {
        if (coarse[w] != 0) {
            return true;
        }
    }
    return false;
}

/* Whether cluster `c` of `mesh` is drawn under `coarse`: when the group
 * simplified into it is coarse (or it is finest) and its own group is not.
 * A NULL `coarse` draws the finest level, as does a mesh without levels. */
static inline bool
r3d_lod_drawn(const r3d_lit_mesh_t* mesh, int c, const uint32_t* coarse) {
    if (mesh->cluster_lod == NULL) {
        return true;
    }
    const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
    if (coarse == NULL) {
        return lod.own == R3D_LOD_NONE;
    }
    return (lod.own == R3D_LOD_NONE || r3d_lod_coarse(coarse, lod.own))
           && (lod.parent == R3D_LOD_NONE || !r3d_lod_coarse(coarse, lod.parent));
}

/* The pixels group `group`'s error spans seen through `lens` (placed with
 * the instance's model), measured where its bounds come nearest the eye and
 * no nearer than the near plane. Scale is the model's largest. */
float r3d_lod_group_pixels(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int group);

/* The share of the picture's height group `group`'s radius spans, projected
 * where r3d_lod_group_pixels() measures. A parent's bounds hold its sources',
 * so it never covers less than one of them. */
float r3d_lod_group_coverage(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int group);

/* Updates `coarse`, R3D_LOD_WORDS(mesh->lod_group_count) words, for one
 * frame seen through `lens`: coarse where a group's error projects within
 * `tolerance_px` and its radius covers at most `coverage_max` of the
 * picture's height. A parent never projects smaller than its sources, in
 * error or radius (r3d_lit_mesh.h checks), and a closing pass sets coarse any
 * source rounding left fine, so the cut never cracks. */
void r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, float tolerance_px, float coverage_max,
                  uint32_t* coarse);
