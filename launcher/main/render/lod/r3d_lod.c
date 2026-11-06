#include "render/lod/r3d_lod.h"
#include "profile/frame_cost.h"

#include "math/geometry/box3f.h"
#include "math/scalar/mathf.h"

#include <string.h>

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

void
r3d_lod_pick(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, float tolerance_px, float coverage_max,
             uint32_t* coarse) {
    const lod_view_t v = lod_view(lens);
    const float enter = tolerance_px - R3D_LOD_HYSTERESIS_PX;
    const float leave = tolerance_px + R3D_LOD_HYSTERESIS_PX;
    FRAME_COST_BEGIN(bound_from);
    const bool none = none_can_coarsen(mesh, &v, lens->project.near_z, leave);
    FRAME_COST_END(bound_from, "lod.bound");
    if (none) {
        memset(coarse, 0, R3D_LOD_WORDS(mesh->lod_group_count) * sizeof *coarse);
        return;
    }
    /* Per group, in the form within() takes: its error and its radius, each
     * times focal over units, against their limits times its nearest depth. */
    const float scale = v.units_per_tick * v.focal;
    const float covered = coverage_max * (float)lens->height;
    const float near_z = lens->project.near_z;
    bool any = false;
    for (int g = 0; g < mesh->lod_group_count; g++) {
        const r3d_lit_lod_group_t* group = &mesh->lod_groups[g];
        const vec3f_t centre = mat4f_apply(&v.view, vec3f_from_vec3s(group->bounds.centre, 1.0F));
        const reach_t at = {vec3f_dot(centre, centre), (float)group->bounds.radius * v.units_per_tick, near_z};
        const float error = (float)group->error * scale;
        const uint32_t bit = 1U << (g % R3D_LOD_WORD_BITS);
        uint32_t* word = &coarse[g / R3D_LOD_WORD_BITS];
        const bool was = (*word & bit) != 0;
        if ((within(&at, error, enter) || (was && within(&at, error, leave)))
            && within(&at, (float)group->bounds.radius * scale, covered)) {
            *word |= bit;
            any = true;
        } else {
            *word &= ~bit;
        }
    }
    /* Nothing coarse closes to nothing. */
    if (any) {
        FRAME_COST_BEGIN(close_from);
        close_coarse(mesh, coarse);
        FRAME_COST_END(close_from, "lod.close");
    }
}
