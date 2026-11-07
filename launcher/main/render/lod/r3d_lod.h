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
 * `lod.hysteresis_px` around `lod.tolerance_px`, so a camera holding still at
 * a switch does not flicker. The bits persist from frame to frame with the
 * instance; all clear is a valid start, every cluster finest.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_lit_mesh.h"
#include "render/r3d_pipeline.h"
#include "render/settings/render_settings.h"

/* The thousandths of `lod.coverage_permille`: 1000 is the picture's whole height. */
#define R3D_LOD_PERMILLE      1000

#define R3D_LOD_WORD_BITS     32
/* The words of coarse bits a mesh's groups need. */
#define R3D_LOD_WORDS(groups) (((groups) + R3D_LOD_WORD_BITS - 1) / R3D_LOD_WORD_BITS)

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
 * `lod.tolerance_px` (`lod.hysteresis_px` either side of it, by the bit's
 * last state) and its radius covers at most `lod.coverage_permille`
 * thousandths of the picture's height. A parent never projects smaller than
 * its sources, in error or radius (r3d_lit_mesh.h checks), and a closing
 * pass sets coarse any source rounding left fine, so the cut never cracks. */
void r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod,
                  uint32_t* coarse);
