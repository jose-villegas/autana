/*
 * r3d_lod: which level of a mesh's clusters an instance draws
 * (docs/render/frame/Levels-of-Detail.md). Once per instance every lod.pick_every frames, r3d_lod_pick()
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

#include "collections/bits.h"
#include "image/pixel/bayer.h"
#include "render/r3d_lit_mesh.h"
#include "render/settings/render_settings.h"

typedef struct r3d_lens r3d_lens_t; /* r3d_pipeline.h */

/* The thousandths of `lod.coverage_permille`: 1000 is the picture's whole height. */
#define R3D_LOD_PERMILLE            1000

/* The most frames lod.pick_every may spread the pick over. */
#define R3D_LOD_PICK_EVERY_MAX      8

/* An instance's level state, R3D_LOD_STATE_SETS sets of bits: its coarse
 * bits, then the blend's arrived and blending bits (r3d_lod_pick()). */
#define R3D_LOD_STATE_SETS          3
#define R3D_LOD_STATE_WORDS(groups) (R3D_LOD_STATE_SETS * BITS32_WORDS(groups))

/* The arrived bits of the state whose coarse bits are `coarse`; the
 * blending bits follow them (r3d_lod_blending()). */
static inline uint32_t*
r3d_lod_arrived(uint32_t* coarse, int groups) {
    return coarse + BITS32_WORDS(groups);
}

/* The blending bits after `arrived`: groups whose blend weighs more than none. */
static inline const uint32_t*
r3d_lod_blending(const uint32_t* arrived, int groups) {
    return arrived + BITS32_WORDS(groups);
}

/* Whether any of a mesh's `groups` is coarse in `coarse`. */
static inline bool
r3d_lod_any(const uint32_t* coarse, int groups) {
    for (int w = 0; w < (int)BITS32_WORDS(groups); w++) {
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
 * simplified into it is coarse (or it is finest), or blending in `blending`
 * when not NULL (the dither draws both levels of a blending group), and the
 * group it is in is not coarse. A NULL `coarse` draws the finest level, as
 * does a mesh without levels. */
static inline bool
r3d_lod_drawn_blending(const r3d_lit_mesh_t* mesh, int c, const uint32_t* coarse, const uint32_t* blending) {
    if (mesh->cluster_lod == NULL) {
        return true;
    }
    const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
    if (coarse == NULL) {
        return lod.own == R3D_LOD_NONE;
    }
    return (lod.own == R3D_LOD_NONE || bits32_get(coarse, lod.own)
            || (blending != NULL && bits32_get(blending, lod.own)))
           && (lod.parent == R3D_LOD_NONE || !bits32_get(coarse, lod.parent));
}

/* r3d_lod_drawn_blending() of one level at a time. */
static inline bool
r3d_lod_drawn(const r3d_lit_mesh_t* mesh, int c, const uint32_t* coarse) {
    return r3d_lod_drawn_blending(mesh, c, coarse, NULL);
}

/* The clusters of `mesh` drawn under `coarse` and `blending`
 * (r3d_lod_drawn_blending()), ascending, into `out`, which holds `room`;
 * returns how many. Nothing coarse or blending lists the finest level
 * without testing each cluster. */
int r3d_lod_list_drawn(const r3d_lit_mesh_t* mesh, const uint32_t* coarse, const uint32_t* blending, uint16_t* out,
                       int room);

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

/* The dither's levels: a 4x4 ordered screen door's thresholds. A blending
 * group's coarse clusters draw where the threshold is under its weight's
 * level, its fine ones where it is not, so the two never share a pixel. */
#define R3D_LOD_DITHER_LEVELS    BAYER4_LEVELS
#define R3D_LOD_DITHER_SHIFT     4
_Static_assert((1 << R3D_LOD_DITHER_SHIFT) == R3D_LOD_DITHER_LEVELS, "a band's nibble holds one level");

/* How many ramps ahead of its weight a group's blending bit is set, so a
 * pick spread over lod.pick_every frames finds a group before its weight
 * rises and never starts a blend partway through its ramp. */
#define R3D_LOD_BLEND_LEAD 1.0F

/* A cluster's dither band packed in a byte: drawn where lo <= level < hi,
 * lo the low nibble, hi R3D_LOD_DITHER_LEVELS less the high nibble. 0 is
 * every level: not dithered. */
static inline int
r3d_lod_dither_lo(uint8_t band) {
    return band & (R3D_LOD_DITHER_LEVELS - 1);
}

static inline int
r3d_lod_dither_hi(uint8_t band) {
    return R3D_LOD_DITHER_LEVELS - (band >> R3D_LOD_DITHER_SHIFT);
}

/* Whether a band draws no level. */
static inline bool
r3d_lod_dither_empty(uint8_t band) {
    return r3d_lod_dither_lo(band) >= r3d_lod_dither_hi(band);
}

/* Whether an instance of `mesh` drawn with lod.blend `blend` dithers: it
 * blends and has no morph targets to geomorph with. */
static inline bool
r3d_lod_dithers(const r3d_lit_mesh_t* mesh, int blend) {
    return blend == RENDER_LOD_BLEND_ON && mesh->morph_delta == NULL;
}

/* The lens's linear part with its focal lengths divided out: position ticks
 * to view units. */
typedef struct {
    mat4f_t view;
    float units_per_tick; /* at least the model's largest scale */
    float focal;          /* pixels a view unit spans at unit depth, the larger axis */
} r3d_lod_view_t;

/* One instance's blend through one lens; `enabled` false draws no blend. */
typedef struct r3d_lod_morph {
    r3d_lod_view_t view; /* set only when enabled */
    float near_z;
    float enter, leave, ramp; /* pixels: whole at enter (leave for an arrived group), none ramp past it */
    /* The pick's arrived bits. Set when
     * a group turns fine, cleared `lod.blend_px` past the hysteresis band: an
     * arrived group ramps from leave, where it came through, any other from
     * enter, where it turns coarse. Both weigh none where the bit changes. */
    const uint32_t* arrived;
    const uint32_t* blending;  /* after `arrived`: a cluster under a group without one weighs none */
    float covered, cover_ramp; /* pixels of radius: whole at covered, none at covered + cover_ramp */
    bool enabled;
    bool dither; /* a screen door stands in for the morph: r3d_lod_dither_band() */
} r3d_lod_morph_t;

/* Whether cluster `c` takes part in the blend: its parent group blends, so
 * it fades out or morphs, or its own group does, so it fades in. One bit
 * test or two, so a draw asks this before it weighs anything. */
static inline bool
r3d_lod_cluster_blends(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c) {
    if (!morph->enabled || mesh->cluster_lod == NULL) {
        return false;
    }
    const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
    return (lod.parent != R3D_LOD_NONE && bits32_get(morph->blending, lod.parent))
           || (lod.own != R3D_LOD_NONE && bits32_get(morph->blending, lod.own));
}

/* Reorders `clusters` so those that blend nothing come first, in their own
 * order, and returns how many: the plain transform and draw take them, the
 * blend's copies only the rest. */
int r3d_lod_morph_partition(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, uint16_t* clusters, int count);

/* The blend of `mesh` seen through `lens`, measured as the pick measures it,
 * `lod` already in the picture's pixels, drawn as `blend`
 * (render_lod_blend_t), with `state` present when blending. Disabled for RENDER_LOD_BLEND_SWITCH; a mesh without
 * morph targets dithers (r3d_lod_dithers()). */
void r3d_lod_morph_init(r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens,
                        const render_lod_settings_t* lod, const uint32_t* state, int blend);

/* Enabled morph's weight for `group`, whether its blending bit is set or
 * not; `pixels`, when present, receives its projected error. */
int r3d_lod_morph_group_weight(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, uint16_t group, float* pixels);

/* Cluster `c`'s weight toward its parent group's coarse surface, 0 to
 * R3D_LOD_MORPH_ONE; 0 without a parent, without a blend, or dithered. */
int r3d_lod_morph_weight(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c);

/* Cluster `c`'s dither band, 0 when not dithered: from its parent group's
 * level when that blends, as it fades out, up to its own group's when that
 * blends, as it fades in. An empty band, lo above hi, is never drawn. */
uint8_t r3d_lod_dither_band(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c);

/* The histogram's bins: half octaves of projected error from
 * 2^R3D_LOD_BIN_LEAST_LOG2 to 2^R3D_LOD_BIN_MOST_LOG2 pixels. */
#define R3D_LOD_BIN_LEAST_LOG2  (-6)
#define R3D_LOD_BIN_MOST_LOG2   8
#define R3D_LOD_BINS_PER_OCTAVE 2
#define R3D_LOD_BINS            ((R3D_LOD_BIN_MOST_LOG2 - R3D_LOD_BIN_LEAST_LOG2) * R3D_LOD_BINS_PER_OCTAVE)

/* What a round of picks saw: per bin, the triangles the groups whose error
 * projects there save drawn coarse (r3d_lit_lod_group_t.saves), and the
 * finest triangles of every instance picked. A group counts at the least
 * tolerance that turns it coarse: its error, or the tolerance whose
 * coverage cap (r3d_lod_steer_coverage()) its radius fits, whichever is
 * more. Below the first bin counts in it; past the last, or under a cap
 * of 0, nowhere. */
typedef struct {
    int32_t saves[R3D_LOD_BINS];
    int32_t finest;
    float unit; /* lod.tolerance_px's pixels per picture pixel: errors bin in the setting's */
} r3d_lod_histogram_t;

/* The steering a raster keeps from round to round. */
typedef struct r3d_lod_steer {
    float tolerance_px;        /* the pick's in lod.tolerance_px's pixels, at least the floor */
    float picked_tolerance_px; /* current picture, before steering the next pick */
    int picked_coverage_permille;
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

/* lod.coverage_permille, `permille` at `floor_px` (above 0), at a steered
 * `tolerance_px`: in proportion to it, so a group too large to turn coarse
 * at the floor turns coarse once the budget asks enough. The floor itself
 * keeps `permille`. */
int r3d_lod_steer_coverage(float tolerance_px, float floor_px, int permille);

/* Updates `state`, R3D_LOD_STATE_WORDS(mesh->lod_group_count) words, for one
 * frame through `lens`: coarse where a group's error projects within
 * `lod.tolerance_px` (± `lod.hysteresis_px` by the bit's last state) and its
 * radius covers at most `lod.coverage_permille` of the picture's height; a
 * closing pass keeps the cut crack-free. With `blend` false only the coarse
 * bits are read or written; a `histogram` takes the saves. True when any
 * bit changed. */
bool r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod, uint32_t* state,
                  bool blend, r3d_lod_histogram_t* histogram);
