#include "render/lod/r3d_lod.h"

#include "math/geometry/box3f.h"
#include "math/scalar/float_bits.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "render/r3d_pipeline.h"

#include <assert.h>
#include <math.h>
#include <string.h>

/* A half octave of error is an octave of its square, whose float exponent
 * names the bin. */
_Static_assert(R3D_LOD_BINS_PER_OCTAVE == 2, "error_bin() reads half octaves off the squared error's exponent");
/* The ratio of a bin's upper edge to its lower: 2^(1/R3D_LOD_BINS_PER_OCTAVE). */
#define BIN_RATIO 1.41421356F

/* The lens's linear part with its focal lengths divided out: position ticks
 * to view units. */
typedef struct {
    mat4f_t view;
    float units_per_tick; /* at least the model's largest scale */
    float focal;          /* pixels a view unit spans at unit depth, the larger axis */
} lod_view_t;

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

static float
group_coverage(const lod_view_t* v, const r3d_lit_lod_group_t* g, float nearest, int height) {
    return (float)g->bounds.radius * v->units_per_tick * v->focal / (nearest * (float)height);
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

/* Adds `saves` to `histogram` at the bin of `length` (in the form within()
 * takes) projected at `at`'s nearest depth, in the histogram's pixels. */
static void
histogram_save(r3d_lod_histogram_t* histogram, const reach_t* at, float length, int saves) {
    const float nearest = mathf_max(mathf_sqrt(at->distance_squared) - at->radius, at->near_z);
    const float px = length * histogram->unit / nearest;
    const int bin = error_bin(px * px);
    if (bin >= 0) {
        histogram->saves[bin] += saves;
    }
}

/* Adds group `group`'s saves to `histogram`, if there is one, at the bin
 * of the least tolerance that turns it coarse at `at`'s nearest depth, in
 * the histogram's pixels: its error, or the tolerance whose cap its radius
 * fits, the cap rising with the tolerance as `covered` over `tolerance` (all
 * in the form within() takes). Under a cap of 0 no tolerance turns it. */
static void
histogram_add(r3d_lod_histogram_t* histogram, const r3d_lit_lod_group_t* group, const reach_t* at, float error,
              float radius, float covered, float tolerance) {
    if (histogram == NULL || covered <= 0.0F) {
        return;
    }
    histogram_save(histogram, at, mathf_max(error, radius * tolerance / covered), group->saves);
}

/* `mesh`'s drop error, its whole bound's radius, in the form within()
 * takes, and in `at` that bound seen through `v`. */
static float
drop_reach(const r3d_lit_mesh_t* mesh, const lod_view_t* v, float near_z, reach_t* at) {
    const box3f_t root = box3f_from_box3s(mesh->nodes[0].bounds, 1.0F);
    const vec3f_t centre = mat4f_apply(&v->view, box3f_centre(root));
    const vec3f_t half = box3f_half_extent(root);
    const float radius = mathf_sqrt(vec3f_dot(half, half)) * v->units_per_tick;
    *at = (reach_t){vec3f_dot(centre, centre), radius, near_z};
    return radius * v->focal;
}

float
r3d_lod_drop_pixels(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens) {
    const lod_view_t v = lod_view(lens);
    reach_t at;
    const float error = drop_reach(mesh, &v, lens->project.near_z, &at);
    return error / mathf_max(mathf_sqrt(at.distance_squared) - at.radius, at.near_z);
}

bool
r3d_lod_pick_drop(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod,
                  int drop_permille, uint32_t* coarse, r3d_lod_histogram_t* histogram) {
    if (drop_permille == 0) {
        return false;
    }
    const lod_view_t v = lod_view(lens);
    const float limit = mathf_min(lod->tolerance_px, (float)drop_permille / R3D_LOD_PERMILLE * lens->height);
    const float enter = limit - lod->hysteresis_px;
    const float leave = limit + lod->hysteresis_px;
    reach_t at;
    const float error = drop_reach(mesh, &v, lens->project.near_z, &at);
    uint32_t* word = &coarse[R3D_LOD_WORDS(mesh->lod_group_count)];
    const bool was = *word != 0;
    const bool dropped = within(&at, error, enter) || (was && within(&at, error, leave));
    *word = dropped ? 1U : 0U;
    if (histogram != NULL) {
        int coarsest = r3d_lod_finest_triangles(mesh);
        for (int g = 0; g < mesh->lod_group_count; g++) {
            coarsest -= mesh->lod_groups[g].saves;
        }
        /* Bin the save at the error, without clamping it to the limit. */
        histogram_save(histogram, &at, error, coarsest);
    }
    return dropped != was;
}

bool
r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const render_lod_settings_t* lod, uint32_t* coarse,
             r3d_lod_histogram_t* histogram) {
    const lod_view_t v = lod_view(lens);
    const float enter = lod->tolerance_px - lod->hysteresis_px;
    const float leave = lod->tolerance_px + lod->hysteresis_px;
    histogram_add_finest(histogram, mesh);
    if (histogram == NULL && none_can_coarsen(mesh, &v, lens->project.near_z, leave)) {
        bool had = false;
        for (int w = 0; w < R3D_LOD_WORDS(mesh->lod_group_count); w++) {
            had |= coarse[w] != 0;
        }
        memset(coarse, 0, R3D_LOD_WORDS(mesh->lod_group_count) * sizeof *coarse);
        return had;
    }
    /* Per group, in the form within() takes: its error and its radius, each
     * times focal over units, against their limits times its nearest depth. */
    const float scale = v.units_per_tick * v.focal;
    const float covered = ((float)lod->coverage_permille / (float)R3D_LOD_PERMILLE) * (float)lens->height;
    const float near_z = lens->project.near_z;
    bool any = false;
    bool changed = false;
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
        histogram_add(histogram, group, &at, error, radius, covered, lod->tolerance_px);
        any |= coarse_now;
        changed |= coarse_now != was;
    }
    /* Nothing coarse closes to nothing, and bits that did not change are as
     * closed as the frame that last closed them left them. */
    if (any && changed) {
        close_coarse(mesh, coarse);
    }
    return changed;
}

int
r3d_lod_list_drawn(const r3d_lit_mesh_t* mesh, const uint32_t* coarse, uint16_t* out, int room) {
    const bool every = r3d_lod_finest_untested(mesh, coarse != NULL && r3d_lod_any(coarse, mesh->lod_group_count));
    const int count = !every || mesh->cluster_lod == NULL ? mesh->cluster_count : mesh->lod_coarse_first;
    int drawn = 0;
    for (int c = 0; c < count; c++) {
        if (every || r3d_lod_drawn(mesh, c, coarse)) {
            assert(drawn < room);
            out[drawn++] = (uint16_t)c;
        }
    }
    (void)room;
    return drawn;
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

/* The least edge from `past_floor` whose prediction in `at_edge`, scaled
 * by `kept`, fits `budget`: -1 when the floor's fits, the coarsest edge when
 * none does. */
static int
edge_within(const int64_t* at_edge, int past_floor, float kept, int budget) {
    if ((float)at_edge[mathi_max(past_floor - 1, 0)] * kept <= (float)budget) {
        return -1;
    }
    for (int edge = past_floor; edge < R3D_LOD_BINS; edge++) {
        if ((float)at_edge[edge] * kept <= (float)budget) {
            return edge;
        }
    }
    return R3D_LOD_BINS; /* out of reach */
}

/* The bin a steer at `from` moves to when `fits` is the least that fits: it
 * rises at once, and falls a bin a round once a bin lower than that fits; one
 * that only just fits is held, so a camera closing in does not dip and climb
 * back. */
static int
steered_bin(int fits, int from, int past_floor) {
    if (fits >= from) {
        return fits;
    }
    const int bin = fits < from - 1 ? from - 1 : from;
    return bin >= 0 && bin < past_floor ? -1 : bin;
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
    const int bin = steered_bin(edge_within(at_edge, past_floor, kept, budget), steer->bin, past_floor);
    steer->settled = bin == steer->bin;
    steer->bin = bin;
    steer->tolerance_px = bin < 0 ? floor_px : mathf_max(r3d_lod_bin_edge(bin), floor_px);
    memset(steer->halves, 0, sizeof steer->halves);
    steer->drawn = 0;
    steer->frames = 0;
}

int
r3d_lod_steer_coverage(float tolerance_px, float floor_px, int permille) {
    const float risen = (float)permille * (tolerance_px / floor_px);
    return risen < (float)INT32_MAX ? (int)risen : INT32_MAX;
}
