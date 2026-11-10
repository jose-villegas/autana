#include "render/lod/r3d_lod.h"

#include "math/geometry/box3f.h"
#include "math/scalar/float_bits.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "render/r3d_pipeline.h"

#include <assert.h>
#include <math.h>
#include <string.h>

typedef r3d_lod_view_t lod_view_t;

/* A half octave of error is an octave of its square, whose float exponent
 * names the bin. */
_Static_assert(R3D_LOD_BINS_PER_OCTAVE == 2, "error_bin() reads half octaves off the squared error's exponent");
/* The ratio of a bin's upper edge to its lower: 2^(1/R3D_LOD_BINS_PER_OCTAVE). */
#define BIN_RATIO 1.41421356F

static lod_view_t
lod_view(const r3d_lens_t* lens) {
    lod_view_t out = {.view = lens->m, .focal = mathf_max(lens->focal_x, lens->focal_y)};
    const float unfocus[2] = {mathf_recip(lens->focal_x), mathf_recip(lens->focal_y)};
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < 4; col++) {
            out.view.m[row][col] *= unfocus[row];
        }
    }
    /* The largest scale bounds every length the view stretches, so a parent
     * group never projects smaller than one inside it: the square root of a
     * bound on the largest eigenvalue of view^T view, its largest absolute
     * row sum, which is exact for a rotation and one scale. */
    float longest = 0.0F;
    for (int i = 0; i < 3; i++) {
        float row = 0.0F;
        for (int j = 0; j < 3; j++) {
            const vec3f_t a = {out.view.m[0][i], out.view.m[1][i], out.view.m[2][i]};
            const vec3f_t b = {out.view.m[0][j], out.view.m[1][j], out.view.m[2][j]};
            row += mathf_abs(vec3f_dot(a, b));
        }
        longest = mathf_max(longest, row);
    }
    out.units_per_tick = mathf_sqrt(longest);
    return out;
}

/* How far group `g`'s bounds come to the eye, no nearer than `near_z`: the
 * depth both its error and its radius project at. */
static float
group_nearest(const lod_view_t* v, const r3d_lit_lod_group_t* g, float near_z) {
    const vec3f_t centre = mat4f_apply(&v->view, vec3f_from_vec3s(g->bounds.centre, 1.0F));
    const float distance = mathf_sqrt(vec3f_dot(centre, centre)) - ((float)g->bounds.radius * v->units_per_tick);
    return mathf_max(distance, near_z);
}

static float
group_pixels(const lod_view_t* v, const r3d_lit_lod_group_t* g, float nearest) {
    return (float)g->error * v->units_per_tick * v->focal / nearest;
}

/* Group `g`'s radius in pixels at `nearest`. */
static float
group_radius_pixels(const lod_view_t* v, const r3d_lit_lod_group_t* g, float nearest) {
    return (float)g->bounds.radius * v->units_per_tick * v->focal / nearest;
}

static float
group_coverage(const lod_view_t* v, const r3d_lit_lod_group_t* g, float nearest, int height) {
    return group_radius_pixels(v, g, nearest) / (float)height;
}

/* The pick's limits in pixels, `scale` times `lod`'s: a group turns coarse
 * under `enter`, stays coarse under `leave`, and its radius covers at most
 * `covered`. */
typedef struct {
    float enter, leave, covered;
} lod_limits_t;

static lod_limits_t
lod_limits(const render_lod_settings_t* lod, const r3d_lens_t* lens, float scale) {
    return (lod_limits_t){(lod->tolerance_px - lod->hysteresis_px) * scale,
                          (lod->tolerance_px + lod->hysteresis_px) * scale,
                          ((float)lod->coverage_permille / (float)R3D_LOD_PERMILLE) * (float)lens->height};
}

float
r3d_lod_group_pixels(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int group) {
    const lod_view_t v = lod_view(lens);
    const r3d_lit_lod_group_t* g = &mesh->lod_groups[group];
    return group_pixels(&v, g, group_nearest(&v, g, lens->project.near_z));
}

float
r3d_lod_group_coverage(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int group) {
    const lod_view_t v = lod_view(lens);
    const r3d_lit_lod_group_t* g = &mesh->lod_groups[group];
    return group_coverage(&v, g, group_nearest(&v, g, lens->project.near_z), lens->height);
}

/* A group's bounds seen through a lod_view_t: the centre's squared distance
 * from the eye, the radius in view units and the near plane, the nearest
 * depth being max(distance - radius, near). */
typedef struct {
    float distance_squared;
    float radius;
    float near_z;
} reach_t;

/* Whether `length` projects within `limit` at the bounds' nearest depth:
 * length <= limit * nearest, both sides squared once the near plane is
 * ruled out, so the frame's pick needs no square root or division. */
static inline bool
within(const reach_t* at, float length, float limit) {
    if (length <= limit * at->near_z) {
        return true;
    }
    const float reach = length + (limit * at->radius);
    return limit >= 0.0F && limit * limit * at->distance_squared >= reach * reach;
}

/* Float rounding of the whole-mesh bound below, as a share of what it
 * compares: far more than the few ulps its sums and the exact test's differ by. */
#define LOD_BOUND_SLACK             (1.0F / 1024.0F)
/* How far a group's centre, rounded to ticks, may sit outside the vertices' bounds, per axis. */
#define GROUP_CENTRE_ROUNDING_TICKS 1.0F

/* Whether no group of `mesh` can be coarse through `v`: its least error, seen
 * from as far as the root's bounds reach, still projects past `leave`. Every
 * group's centre lies inside those bounds, give or take its rounding, so none
 * comes farther. */
static bool
none_can_coarsen(const r3d_lit_mesh_t* mesh, const lod_view_t* v, float near_z, float leave) {
    if (mesh->lod_error_least == 0) {
        return false;
    }
    const box3f_t root = box3f_from_box3s(mesh->nodes[0].bounds, 1.0F);
    const vec3f_t centre = mat4f_apply(&v->view, box3f_centre(root));
    const vec3f_t half =
        vec3f_add(box3f_half_extent(root),
                  (vec3f_t){GROUP_CENTRE_ROUNDING_TICKS, GROUP_CENTRE_ROUNDING_TICKS, GROUP_CENTRE_ROUNDING_TICKS});
    const float farthest = mathf_max(
        mathf_sqrt(vec3f_dot(centre, centre)) + (mathf_sqrt(vec3f_dot(half, half)) * v->units_per_tick), near_z);
    const float px = (float)mesh->lod_error_least * v->units_per_tick * v->focal / farthest;
    return px > leave * (1.0F + LOD_BOUND_SLACK);
}

/* Sets coarse every group simplified into a coarse one, until none is left
 * fine under a coarse parent. The ordering r3d_lit_mesh.h checks makes this
 * a pass that changes nothing, short of float rounding far off; then the cut
 * still never draws a group both coarse and fine. */
static void
close_coarse(const r3d_lit_mesh_t* mesh, uint32_t* coarse) {
    bool changed = true;
    for (int pass = 0; changed && pass < mesh->lod_group_count; pass++) {
        changed = false;
        for (int c = 0; c < mesh->cluster_count; c++) {
            const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
            if (lod.own != R3D_LOD_NONE && lod.parent != R3D_LOD_NONE && r3d_lod_coarse(coarse, lod.parent)
                && !r3d_lod_coarse(coarse, lod.own)) {
                coarse[lod.own / R3D_LOD_WORD_BITS] |= 1U << (lod.own % R3D_LOD_WORD_BITS);
                changed = true;
            }
        }
    }
}

void
r3d_lod_morph_init(r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens,
                   const render_lod_settings_t* lod, float pixels_per_output_pixel, const uint32_t* arrived,
                   int blend) {
    const uint32_t* blending = arrived != NULL ? r3d_lod_blending(arrived, mesh->lod_group_count) : NULL;
    /* Off, or no group blending: nothing weighs, and the transform and draw
     * take their plain paths. */
    if (blend != RENDER_LOD_BLEND_ON || (blending != NULL && !r3d_lod_any(blending, mesh->lod_group_count))) {
        *morph = (r3d_lod_morph_t){.enabled = false};
        return;
    }
    const lod_limits_t limits = lod_limits(lod, lens, pixels_per_output_pixel);
    *morph = (r3d_lod_morph_t){
        .view = lod_view(lens),
        .near_z = lens->project.near_z,
        .enter = limits.enter,
        .leave = limits.leave,
        .arrived = arrived,
        .blending = blending,
        .ramp = lod->blend_px * pixels_per_output_pixel,
        .covered = limits.covered,
        .cover_ramp = limits.covered * R3D_LOD_MORPH_COVER_RAMP,
        .enabled = true,
        .dither = r3d_lod_dithers(mesh, blend),
    };
}

/* 1 at or below `whole`, 0 at or past `whole + ramp`, linear between. */
static float
ramp_weight(float value, float whole, float ramp) {
    if (value <= whole) {
        return 1.0F;
    }
    if (value >= whole + ramp) {
        return 0.0F;
    }
    return (whole + ramp - value) / ramp;
}

/* Group `g`'s error and radius projected where the pick measures them, in
 * the blend's pixels. */
static float
group_morph_pixels(const r3d_lod_morph_t* morph, const r3d_lit_lod_group_t* g, float* radius_px) {
    const float nearest = group_nearest(&morph->view, g, morph->near_z);
    *radius_px = group_radius_pixels(&morph->view, g, nearest);
    return group_pixels(&morph->view, g, nearest);
}

static int
group_morph_weight(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, uint16_t group) {
    const r3d_lit_lod_group_t* g = &mesh->lod_groups[group];
    float radius_px = 0.0F;
    const float error_px = group_morph_pixels(morph, g, &radius_px);
    const float whole = morph->arrived == NULL || r3d_lod_coarse(morph->arrived, group) ? morph->leave : morph->enter;
    const float weight =
        mathf_min(ramp_weight(error_px, whole, morph->ramp), ramp_weight(radius_px, morph->covered, morph->cover_ramp));
    return (int)((weight * (float)R3D_LOD_MORPH_ONE) + 0.5F);
}

/* Group `group`'s weight when it blends, else 0. */
static int
blending_weight(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, uint16_t group) {
    if (group == R3D_LOD_NONE || (morph->blending != NULL && !r3d_lod_coarse(morph->blending, group))) {
        return 0;
    }
    return group_morph_weight(morph, mesh, group);
}

int
r3d_lod_morph_weight(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c) {
    if (!morph->enabled || morph->dither) {
        return 0;
    }
    return blending_weight(morph, mesh, mesh->cluster_lod[c].parent);
}

/* The levels a weight covers: those whose threshold, at the middle of its
 * step, is under it. */
static int
dither_level(int weight) {
    return ((2 * weight * R3D_LOD_DITHER_LEVELS) + R3D_LOD_MORPH_ONE - 1) / (2 * R3D_LOD_MORPH_ONE);
}

int
r3d_lod_morph_partition(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, uint16_t* clusters, int count) {
    int plain = 0;
    for (int i = 0; i < count; i++) {
        if (!r3d_lod_cluster_blends(morph, mesh, clusters[i])) {
            const uint16_t c = clusters[i];
            clusters[i] = clusters[plain];
            clusters[plain++] = c;
        }
    }
    return plain;
}

uint8_t
r3d_lod_dither_band(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c) {
    if (!morph->enabled || !morph->dither || mesh->cluster_lod == NULL) {
        return 0;
    }
    const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[c];
    /* A cluster simplified from a group still fine is drawn only while that group blends. */
    const bool fading_in =
        lod.own != R3D_LOD_NONE && morph->blending != NULL && r3d_lod_coarse(morph->blending, lod.own);
    const int lo = dither_level(blending_weight(morph, mesh, lod.parent));
    const int hi = fading_in ? dither_level(group_morph_weight(morph, mesh, lod.own)) : R3D_LOD_DITHER_LEVELS;
    if (lo >= hi) {
        return (uint8_t)((R3D_LOD_DITHER_LEVELS - 1) | ((R3D_LOD_DITHER_LEVELS - 1) << R3D_LOD_DITHER_SHIFT));
    }
    return (uint8_t)(lo | ((R3D_LOD_DITHER_LEVELS - hi) << R3D_LOD_DITHER_SHIFT));
}

float
r3d_lod_morph_stacked_px(const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int c) {
    if (r3d_lod_morph_weight(morph, mesh, c) == 0) {
        return 0.0F;
    }
    const uint16_t parent = mesh->cluster_lod[c].parent;
    float most = 0.0F;
    for (int k = 0; k < mesh->cluster_count; k++) {
        const r3d_lit_cluster_lod_t lod = mesh->cluster_lod[k];
        if (lod.own != parent || lod.parent == R3D_LOD_NONE) {
            continue;
        }
        const r3d_lit_lod_group_t* g = &mesh->lod_groups[lod.parent];
        float radius_px = 0.0F;
        const float error_px = group_morph_pixels(morph, g, &radius_px);
        const float weight = (float)group_morph_weight(morph, mesh, lod.parent) / (float)R3D_LOD_MORPH_ONE;
        most = mathf_max(most, weight * error_px);
    }
    return most;
}

/* Sets group `g`'s bit in `bits` to `on`; whether it changed. */
static bool
set_bit(uint32_t* bits, int g, bool on) {
    const uint32_t bit = 1U << (g % R3D_LOD_WORD_BITS);
    const uint32_t before = bits[g / R3D_LOD_WORD_BITS];
    bits[g / R3D_LOD_WORD_BITS] = on ? before | bit : before & ~bit;
    return bits[g / R3D_LOD_WORD_BITS] != before;
}

/* Whether fine `group`'s blend may weigh more than none by the next pick, its
 * error ramping from `whole`: both its error and its radius inside their
 * ramps and R3D_LOD_BLEND_LEAD more, by LOD_BOUND_SLACK more so the blend's
 * own measure never finds one outside. */
static bool
blends(const reach_t* at, const r3d_lit_lod_group_t* group, float scale, float covered,
       const render_lod_settings_t* lod, float whole) {
    const float slack = 1.0F + LOD_BOUND_SLACK;
    const float ramps = 1.0F + R3D_LOD_BLEND_LEAD;
    return within(at, (float)group->error * scale, (whole + (lod->blend_px * ramps)) * slack)
           && within(at, (float)group->bounds.radius * scale,
                     covered * (1.0F + (R3D_LOD_MORPH_COVER_RAMP * ramps)) * slack);
}

float
r3d_lod_bin_edge(int edge) {
    return ldexpf((edge % R3D_LOD_BINS_PER_OCTAVE) != 0 ? BIN_RATIO : 1.0F,
                  R3D_LOD_BIN_LEAST_LOG2 + (edge / R3D_LOD_BINS_PER_OCTAVE));
}

/* The bin of a group whose error projects to sqrt(px_squared) pixels, or -1
 * past the last: below the first edge counts in the first. */
static int
error_bin(float px_squared) {
    const float least = r3d_lod_bin_edge(0);
    if (px_squared < least * least) {
        return 0;
    }
    /* px_squared is in [2^(exponent - 1), 2^exponent) */
    const int bin = mathf_exponent(px_squared) - 1 - (R3D_LOD_BIN_LEAST_LOG2 * R3D_LOD_BINS_PER_OCTAVE);
    return bin < R3D_LOD_BINS ? bin : -1;
}

/* Adds `mesh`'s finest triangles to `histogram`, if there is one. */
static void
histogram_add_finest(r3d_lod_histogram_t* histogram, const r3d_lit_mesh_t* mesh) {
    if (histogram != NULL) {
        histogram->finest += r3d_lod_finest_triangles(mesh);
    }
}

/* Adds group `group`'s saves to `histogram`, if there is one, at the bin
 * its error, `error` in the form within() takes, projects to at `at`'s
 * nearest depth, in the histogram's pixels. A group whose radius spans past
 * `covered` never turns coarse, so saves nothing; `coarse_now` says it did. */
static void
histogram_add(r3d_lod_histogram_t* histogram, const r3d_lit_lod_group_t* group, const reach_t* at, float error,
              bool coarse_now, float radius, float covered) {
    if (histogram == NULL || !(coarse_now || within(at, radius, covered))) {
        return;
    }
    const float nearest = mathf_max(mathf_sqrt(at->distance_squared) - at->radius, at->near_z);
    const float px = error * histogram->unit / nearest;
    const int bin = error_bin(px * px);
    if (bin >= 0) {
        histogram->saves[bin] += group->saves;
    }
}

/* Inlined into one copy per caller, so a literal NULL `arrived` keeps no
 * blend state. */
static inline __attribute__((always_inline)) bool
pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod, uint32_t* coarse,
     uint32_t* arrived, r3d_lod_histogram_t* histogram) {
    const lod_view_t v = lod_view(lens);
    const lod_limits_t limits = lod_limits(lod, lens, 1.0F);
    const float enter = limits.enter;
    const float leave = limits.leave;
    const int words = R3D_LOD_WORDS(mesh->lod_group_count);
    uint32_t* blending = arrived != NULL ? arrived + words : NULL;
    /* With a blend, a group may weigh something until its error is past
     * leave by a ramp and its lead; nearer than that, every bit clears. */
    const float reach = arrived != NULL ? lod->blend_px * (1.0F + R3D_LOD_BLEND_LEAD) : 0.0F;
    histogram_add_finest(histogram, mesh);
    if (histogram == NULL && none_can_coarsen(mesh, &v, lens->project.near_z, leave + reach)) {
        assert(arrived == NULL || arrived == coarse + words);
        const int state = (arrived != NULL ? R3D_LOD_STATE_SETS : 1) * words;
        bool had = false;
        for (int w = 0; w < state; w++) {
            had |= coarse[w] != 0;
        }
        memset(coarse, 0, (size_t)state * sizeof *coarse);
        return had;
    }
    /* Per group, in the form within() takes: its error and its radius, each
     * times focal over units, against their limits times its nearest depth. */
    const float scale = v.units_per_tick * v.focal;
    const float covered = limits.covered;
    const float near_z = lens->project.near_z;
    bool any = false;
    bool changed = false;       /* a coarse bit */
    bool blend_changed = false; /* an arrived or blending bit */
    for (int g = 0; g < mesh->lod_group_count; g++) {
        const r3d_lit_lod_group_t* group = &mesh->lod_groups[g];
        const vec3f_t centre = mat4f_apply(&v.view, vec3f_from_vec3s(group->bounds.centre, 1.0F));
        const reach_t at = {vec3f_dot(centre, centre), (float)group->bounds.radius * v.units_per_tick, near_z};
        const float error = (float)group->error * scale;
        const uint32_t bit = 1U << (g % R3D_LOD_WORD_BITS);
        uint32_t* word = &coarse[g / R3D_LOD_WORD_BITS];
        const bool was = (*word & bit) != 0;
        const float radius = (float)group->bounds.radius * scale;
        const bool coarse_now =
            (within(&at, error, enter) || (was && within(&at, error, leave))) && within(&at, radius, covered);
        *word = coarse_now ? *word | bit : *word & ~bit;
        histogram_add(histogram, group, &at, error, coarse_now, radius, covered);
        any |= coarse_now;
        changed |= coarse_now != was;
        if (!coarse_now && arrived != NULL) {
            const uint32_t before = arrived[g / R3D_LOD_WORD_BITS];
            /* Past leave by a ramp it has arrived nowhere, even turning fine
             * this pick: setting the bit would only clear it on the next. */
            if (!within(&at, error, leave + lod->blend_px)) {
                arrived[g / R3D_LOD_WORD_BITS] &= ~bit;
            } else if (was) {
                arrived[g / R3D_LOD_WORD_BITS] |= bit;
            }
            blend_changed |= before != arrived[g / R3D_LOD_WORD_BITS];
            blend_changed |= set_bit(
                blending, g,
                blends(&at, group, scale, covered, lod, (arrived[g / R3D_LOD_WORD_BITS] & bit) != 0 ? leave : enter));
        }
    }
    /* Nothing coarse closes to nothing, and bits that did not change are as
     * closed as the frame that last closed them left them. */
    if (any && changed) {
        close_coarse(mesh, coarse);
    }
    /* A coarse group has not arrived, closed coarse or not. */
    for (int w = 0; arrived != NULL && w < words; w++) {
        const uint32_t kept_arrived = arrived[w] & ~coarse[w];
        const uint32_t kept_blending = blending[w] & ~coarse[w];
        blend_changed |= kept_arrived != arrived[w] || kept_blending != blending[w];
        arrived[w] = kept_arrived;
        blending[w] = kept_blending;
    }
    return changed || blend_changed;
}

bool
r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod, uint32_t* coarse,
             uint32_t* arrived, r3d_lod_histogram_t* histogram) {
    return arrived == NULL ? pick(mesh, lens, lod, coarse, NULL, histogram)
                           : pick(mesh, lens, lod, coarse, arrived, histogram);
}

/* Inlined into one copy per caller, so a literal NULL `blending` tests no
 * blend. */
static inline __attribute__((always_inline)) int
list_drawn(const r3d_lit_mesh_t* mesh, const uint32_t* coarse, const uint32_t* blending, uint16_t* out, int room) {
    const bool any = coarse != NULL
                     && (r3d_lod_any(coarse, mesh->lod_group_count)
                         || (blending != NULL && r3d_lod_any(blending, mesh->lod_group_count)));
    const bool every = r3d_lod_finest_untested(mesh, any);
    const int count = !every || mesh->cluster_lod == NULL ? mesh->cluster_count : mesh->lod_coarse_first;
    int drawn = 0;
    for (int c = 0; c < count; c++) {
        if (every || r3d_lod_drawn_blending(mesh, c, coarse, blending)) {
            assert(drawn < room);
            out[drawn++] = (uint16_t)c;
        }
    }
    (void)room;
    return drawn;
}

int
r3d_lod_list_drawn(const r3d_lit_mesh_t* mesh, const uint32_t* coarse, const uint32_t* blending, uint16_t* out,
                   int room) {
    return blending == NULL ? list_drawn(mesh, coarse, NULL, out, room) : list_drawn(mesh, coarse, blending, out, room);
}

void
r3d_lod_steer_reset(r3d_lod_steer_t* steer, float floor_px) {
    memset(steer, 0, sizeof *steer);
    steer->tolerance_px = floor_px;
    steer->bin = -1;
}

/* The least edge, 0 .. R3D_LOD_BINS + 1, past `px`. */
static int
edge_past(float px) {
    int edge = 0;
    while (edge <= R3D_LOD_BINS && r3d_lod_bin_edge(edge) <= px) {
        edge++;
    }
    return edge;
}

void
r3d_lod_steer_frame(r3d_lod_steer_t* steer, float floor_px, int budget, int drawn, bool round_done) {
    steer->drawn += drawn;
    steer->frames++;
    if (!round_done) {
        return;
    }
    /* at_edge[e]: the triangles predicted at bin edge e's tolerance, the
     * finest less the saves of every bin wholly within it; a tolerance
     * between edges predicts its lower edge's. */
    int64_t at_edge[R3D_LOD_BINS + 1];
    at_edge[0] = (int64_t)steer->halves[0].finest + steer->halves[1].finest;
    for (int b = 0; b < R3D_LOD_BINS; b++) {
        at_edge[b + 1] = at_edge[b] - steer->halves[0].saves[b] - steer->halves[1].saves[b];
    }
    const int past_floor = edge_past(floor_px);
    /* What culling and the cut's lag leave of the prediction, measured at
     * the tolerance the round drew with. */
    const int64_t was = at_edge[mathi_max(edge_past(steer->tolerance_px) - 1, 0)];
    const float kept = was > 0 ? (float)steer->drawn / ((float)steer->frames * (float)was) : 1.0F;
    int bin = -1;
    if ((float)at_edge[mathi_max(past_floor - 1, 0)] * kept > (float)budget) {
        bin = R3D_LOD_BINS; /* out of reach: the coarsest edge */
        for (int edge = past_floor; edge < R3D_LOD_BINS; edge++) {
            if ((float)at_edge[edge] * kept <= (float)budget) {
                bin = edge;
                break;
            }
        }
    }
    if (bin < steer->bin - 1) {
        bin = steer->bin - 1; /* falls a bin a round */
        bin = bin >= 0 && bin < past_floor ? -1 : bin;
    }
    steer->settled = bin == steer->bin;
    steer->bin = bin;
    steer->tolerance_px = bin < 0 ? floor_px : mathf_max(r3d_lod_bin_edge(bin), floor_px);
    memset(steer->halves, 0, sizeof steer->halves);
    steer->drawn = 0;
    steer->frames = 0;
}
