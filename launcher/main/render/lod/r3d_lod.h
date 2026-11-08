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
#define R3D_LOD_PERMILLE            1000

#define R3D_LOD_WORD_BITS           32
/* The words of coarse bits a mesh's groups need. */
#define R3D_LOD_WORDS(groups)       (((groups) + R3D_LOD_WORD_BITS - 1) / R3D_LOD_WORD_BITS)
/* An instance's level state, R3D_LOD_STATE_SETS sets of bits: its coarse
 * bits, then the blend's arrived and blending bits (r3d_lod_pick()). */
#define R3D_LOD_STATE_SETS          3
#define R3D_LOD_STATE_WORDS(groups) (R3D_LOD_STATE_SETS * R3D_LOD_WORDS(groups))

/* The arrived bits of the state whose coarse bits are `coarse`; the
 * blending bits follow them (r3d_lod_blending()). */
static inline uint32_t*
r3d_lod_arrived(uint32_t* coarse, int groups) {
    return coarse + R3D_LOD_WORDS(groups);
}

/* The blending bits after `arrived`: groups whose blend weighs more than none. */
static inline const uint32_t*
r3d_lod_blending(const uint32_t* arrived, int groups) {
    return arrived + R3D_LOD_WORDS(groups);
}

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

/* Geomorphing (render/lmor_format.h): a drawn cluster with a parent group
 * blends toward its targets on that group's coarse surface. The weight is
 * whole at the group's next switch, so a switch moves nothing, and none
 * `lod.blend_px` past it; for the coverage cap, whole at the cap, none
 * R3D_LOD_MORPH_COVER_RAMP of it above. The lesser weight is drawn. */
#define R3D_LOD_MORPH_COVER_RAMP 0.25F
/* A weight's whole: the transform's fixed-point unit. */
#define R3D_LOD_MORPH_ONE        128

/* One instance's blend through one lens; `enabled` false draws no blend. */
typedef struct r3d_lod_morph {
    mat4f_t view; /* the lens's linear part, focal lengths divided out */
    float units_per_tick, scale, near_z;
    float enter, leave, ramp; /* pixels: whole at enter (leave for an arrived group), none ramp past it */
    /* The pick's arrived bits, or NULL: every group ramps from leave. Set when
     * a group turns fine, cleared `lod.blend_px` past the hysteresis band: an
     * arrived group ramps from leave, where it came through, any other from
     * enter, where it turns coarse. Both weigh none where the bit changes. */
    const uint32_t* arrived;
    const uint32_t* blending;  /* after `arrived`, or NULL: a cluster under a group without one weighs none */
    float covered, cover_ramp; /* pixels of radius: whole at covered, none at covered + cover_ramp */
    bool enabled;
} r3d_lod_morph_t;

/* The blend of `mesh` seen through `lens`, measured as the pick measures it,
 * its thresholds `pixels_per_output_pixel` times `lod`'s. Disabled for a mesh
 * without morph targets or when `enabled` is false. */
void r3d_lod_morph_init(r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens,
                        const render_lod_settings_t* lod, float pixels_per_output_pixel, const uint32_t* arrived,
                        bool enabled);

/* Cluster `c`'s weight toward its parent group's coarse surface, 0 to
 * R3D_LOD_MORPH_ONE; 0 without a parent or a blend. */
int r3d_lod_morph_weight(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c);

/* A bound, in the blend's pixels, on how far cluster `c`'s targets sit from
 * the coarse surface as it draws: its targets lie on its parent's coarse
 * clusters unblended, while those clusters may be blending toward their own
 * parent. 0 when `c` is not blending or its target level is not. For
 * measurement: it walks every cluster. */
float r3d_lod_morph_stacked_px(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c);

/* Updates `coarse`, R3D_LOD_WORDS(mesh->lod_group_count) words, for one
 * frame through `lens`: coarse where a group's error projects within
 * `lod.tolerance_px` (± `lod.hysteresis_px` by the bit's last state) and its
 * radius covers at most `lod.coverage_permille` of the picture's height.
 * Parents never project smaller than sources and a closing pass sets
 * coarse what rounding left fine, so the cut never cracks. `arrived`, twice
 * as many words or NULL, keeps the blend's arrived and blending bits. */
void r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod,
                  uint32_t* coarse, uint32_t* arrived);
