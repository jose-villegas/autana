/*
 * r3d_lod: which level of a mesh's clusters an instance draws
 * (docs/plans/Cluster-LOD.md). Once per instance every lod.pick_every frames, r3d_lod_pick()
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
 *
 * With `lod.triangle_budget` set, the tolerance the pick uses is steered:
 * each pick adds what its groups would save drawn coarse to a histogram over
 * their projected error, and when a round of picks has seen every instance,
 * r3d_lod_steer_frame() reads off the least tolerance, from `lod.tolerance_px` up,
 * that draws within the budget.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "render/r3d_lit_mesh.h"
#include "render/settings/render_settings.h"

typedef struct r3d_lens r3d_lens_t; /* r3d_pipeline.h */

/* The thousandths of `lod.coverage_permille`: 1000 is the picture's whole height. */
#define R3D_LOD_PERMILLE       1000

/* The most frames lod.pick_every may spread the pick over. */
#define R3D_LOD_PICK_EVERY_MAX 8

#define R3D_LOD_WORD_BITS      32
/* The words of coarse bits a mesh's groups need. */
#define R3D_LOD_WORDS(groups)  (((groups) + R3D_LOD_WORD_BITS - 1) / R3D_LOD_WORD_BITS)

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

/* Whether, with `any_coarse` false, every cluster drawn is known without a
 * test: all of a mesh without levels, or the finest, which levels laid out
 * apart (lod_finest_root set) put first, up to lod_coarse_first. */
static inline bool
r3d_lod_finest_untested(const r3d_lit_mesh_t* mesh, bool any_coarse) {
    return !any_coarse && (mesh->lod_finest_root != 0 || mesh->cluster_lod == NULL);
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

/* The clusters of `mesh` drawn under `coarse` (r3d_lod_drawn()), ascending,
 * into `out`, which holds `room`; returns how many. Nothing coarse lists the
 * finest level without testing each cluster. */
int r3d_lod_list_drawn(const r3d_lit_mesh_t* mesh, const uint32_t* coarse, uint16_t* out, int room);

/* The pixels group `group`'s error spans seen through `lens` (placed with
 * the instance's model), measured where its bounds come nearest the eye and
 * no nearer than the near plane. Scale is the model's largest. */
float r3d_lod_group_pixels(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int group);

/* The share of the picture's height group `group`'s radius spans, projected
 * where r3d_lod_group_pixels() measures. A parent's bounds hold its sources',
 * so it never covers less than one of them. */
float r3d_lod_group_coverage(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int group);

/* The histogram's bins: half octaves of projected error from
 * 2^R3D_LOD_BIN_LEAST_LOG2 to 2^R3D_LOD_BIN_MOST_LOG2 pixels. */
#define R3D_LOD_BIN_LEAST_LOG2  (-6)
#define R3D_LOD_BIN_MOST_LOG2   8
#define R3D_LOD_BINS_PER_OCTAVE 2
#define R3D_LOD_BINS            ((R3D_LOD_BIN_MOST_LOG2 - R3D_LOD_BIN_LEAST_LOG2) * R3D_LOD_BINS_PER_OCTAVE)

/* What a round of picks saw: per bin, the triangles the groups whose error
 * projects there save drawn coarse (r3d_lit_lod_group_t.saves), and the
 * finest triangles of every instance picked. Error below the first bin
 * counts in it; error past the last, or a group too large to turn coarse,
 * counts nowhere. */
typedef struct {
    int32_t saves[R3D_LOD_BINS];
    int32_t finest;
    float unit; /* lod.tolerance_px's pixels per picture pixel: errors bin in the setting's */
} r3d_lod_histogram_t;

/* The steering a raster keeps from round to round. */
typedef struct r3d_lod_steer {
    float tolerance_px;            /* the pick's in lod.tolerance_px's pixels, at least the floor */
    int bin;                       /* the bin edge tolerance_px sits at, or -1 at the floor */
    r3d_lod_histogram_t halves[2]; /* this round's, one per core of the cull */
    int64_t drawn;                 /* triangles the round's frames drew */
    int frames;                    /* frames in the round so far */
    bool settled;                  /* the last round kept the tolerance: no picks are owed to it */
} r3d_lod_steer_t;

/* The tolerance bin edge `edge` stands for, 0 .. R3D_LOD_BINS: bin
 * `edge`'s lower bound, R3D_LOD_BINS the last one's upper. */
float r3d_lod_bin_edge(int edge);

/* The finest level's triangles of `mesh`: all of a mesh without levels. */
static inline int
r3d_lod_finest_triangles(const r3d_lit_mesh_t* mesh) {
    return mesh->cluster_lod != NULL ? mesh->lod_finest_triangles : mesh->triangle_count;
}

/* A steer at `floor_px`, its round empty: a scene's or view's first. */
void r3d_lod_steer_reset(r3d_lod_steer_t* steer, float floor_px);

/* Ends a frame of the round: adds the triangles it drew and, once the round
 * has seen every instance (`round_done`), sets the tolerance for the next
 * round and empties it. The predicted triangles at each edge, scaled by
 * what the frames drew over what was predicted at the tolerance they drew
 * with, choose the least edge past `floor_px` within `budget`; the
 * tolerance rises to it at once but falls at most a bin a round, so a view
 * held still settles rather than swings. Pixels are lod.tolerance_px's. */
void r3d_lod_steer_frame(r3d_lod_steer_t* steer, float floor_px, int budget, int drawn, bool round_done);

/* Updates `coarse`, R3D_LOD_WORDS(mesh->lod_group_count) words, for one
 * frame seen through `lens`: coarse where a group's error projects within
 * `lod.tolerance_px` (`lod.hysteresis_px` either side, by the bit's last
 * state) and its radius spans at most `lod.coverage_permille` thousandths
 * of the picture's height. Parents never project smaller than their sources
 * (r3d_lit_mesh.h) and a closing pass catches rounding, so the cut never
 * cracks. True when a bit changed. A `histogram` takes the saves. */
bool r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod,
                  uint32_t* coarse, r3d_lod_histogram_t* histogram);
