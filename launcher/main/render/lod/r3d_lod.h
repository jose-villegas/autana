/*
 * r3d_lod: which level of a mesh's clusters an instance draws
 * (docs/plans/Cluster-LOD.md). Once per instance a frame, r3d_lod_pick()
 * sets one bit per group: coarse when the group's error, projected at its
 * bounds' nearest point, is within the tolerance. The cluster walk then keeps
 * a cluster when r3d_lod_drawn() says so, before its box test.
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
#define R3D_LOD_TOLERANCE_PX  2.0F
/* A coarse group stays coarse until its error passes the tolerance by this
 * much, and a fine one turns coarse once it is this much under. */
#define R3D_LOD_HYSTERESIS_PX 0.5F

#define R3D_LOD_WORD_BITS     32
/* The words of coarse bits a mesh's groups need. */
#define R3D_LOD_WORDS(groups) (((groups) + R3D_LOD_WORD_BITS - 1) / R3D_LOD_WORD_BITS)

static inline bool
r3d_lod_coarse(const uint32_t* coarse, uint16_t group) {
    return ((coarse[group / R3D_LOD_WORD_BITS] >> (group % R3D_LOD_WORD_BITS)) & 1U) != 0;
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

/* Updates `coarse`, R3D_LOD_WORDS(mesh->lod_group_count) words, for one
 * frame seen through `lens`. A parent group never projects smaller than a
 * group simplified into it (r3d_lit_mesh.h checks the bounds and errors), and
 * a closing pass sets coarse any source rounding left fine, so a coarse
 * group's sources are always coarse and the cut never cracks. */
void r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, float tolerance_px, uint32_t* coarse);
