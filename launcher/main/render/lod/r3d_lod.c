#include "render/lod/r3d_lod.h"

#include "math/scalar/mathf.h"

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
    for (int g = 0; g < mesh->lod_group_count; g++) {
        const r3d_lit_lod_group_t* group = &mesh->lod_groups[g];
        const float nearest = group_nearest(&v, group, lens->project.near_z);
        const float px = group_pixels(&v, group, nearest);
        const uint32_t bit = 1U << (g % R3D_LOD_WORD_BITS);
        uint32_t* word = &coarse[g / R3D_LOD_WORD_BITS];
        const bool was = (*word & bit) != 0;
        const bool small = group_coverage(&v, group, nearest, lens->height) <= coverage_max;
        if (small && (px <= enter || (was && px <= leave))) {
            *word |= bit;
        } else {
            *word &= ~bit;
        }
    }
    close_coarse(mesh, coarse);
}
