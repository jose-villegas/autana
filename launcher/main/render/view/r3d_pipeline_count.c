#include "render/view/r3d_pipeline_count.h"

#include <assert.h>

#include "math/geometry/box3f.h"
#include "math/geometry/planef.h"
#include "render/r3d_triangle_tests.h"

#define ALL_PLANES ((1u << PLANE_FRUSTUM_COUNT) - 1u)

/* r3d_pipeline_cull()'s tests without its nearest-first order, which no count
 * depends on; the cull-view suite holds the two to the same clusters. */
int
r3d_pipeline_cull_counted(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, uint16_t* out,
                          r3d_pipeline_count_walk_t* walk, r3d_pipeline_counts_t* counts) {
    planef_frustum(&lens->m, &lens->project, rectf_from_edges(0.0F, 0.0F, (float)lens->width, (float)lens->height),
                   walk->planes);
    int top = 0;
    walk->node[top] = 0;
    walk->mask[top++] = (uint8_t)ALL_PLANES;
    int count = 0;
    while (top > 0) {
        top--;
        const r3d_lit_node_t* node = &mesh->nodes[walk->node[top]];
        unsigned mask = walk->mask[top];
        counts->nodes_visited++;
        if (mask != 0
            && box3f_outside_planes(box3f_from_box3s(node->bounds, 1.0F), walk->planes, PLANE_FRUSTUM_COUNT, &mask)) {
            counts->nodes_cut++;
            continue;
        }
        if (!node->leaf) {
            assert(top + node->count <= R3D_LIT_WALK_STACK_MAX);
            for (int i = 0; i < node->count; i++) {
                walk->node[top] = (uint16_t)(node->first + i);
                walk->mask[top++] = (uint8_t)mask;
            }
            continue;
        }
        for (int i = 0; i < node->count; i++) {
            const int c = node->first + i;
            unsigned cluster_mask = mask;
            if (cluster_mask == 0
                || !box3f_outside_planes(box3f_from_box3s(mesh->clusters[c].bounds, 1.0F), walk->planes,
                                         PLANE_FRUSTUM_COUNT, &cluster_mask)) {
                out[count++] = (uint16_t)c;
                counts->triangles_submitted += mesh->clusters[c].triangle_count;
            }
        }
        counts->clusters_tested += node->count;
    }
    counts->clusters_kept += count;
    return count;
}

/* One triangle into its counter by the draw's own sort (r3d_pipeline.c,
 * draw_triangles and draw_in_front): every corner in front, then some corner
 * in front; one wholly in front is then missed, facing away or set up. True
 * when it reaches r3d_span. The cull-view suite holds the set-up count to
 * the triangles the draw fills. */
static bool
count_triangle(const r3d_pipeline_vertex_t* const v[3], bool double_sided, const r3d_span_target_t* screen,
               r3d_pipeline_counts_t* counts) {
    if (!(v[0]->iz > 0.0F && v[1]->iz > 0.0F && v[2]->iz > 0.0F)) {
        const bool clipped = v[0]->iz != 0.0F || v[1]->iz != 0.0F || v[2]->iz != 0.0F;
        *(clipped ? &counts->triangles_clipped : &counts->triangles_behind) += 1;
        return clipped;
    }
    if (misses_every_centre(v[0], v[1], v[2], screen)) {
        counts->triangles_missed++;
        return false;
    }
    if (facing_away(signed_area2(v[0]->sx, v[0]->sy, v[1]->sx, v[1]->sy, v[2]->sx, v[2]->sy), double_sided)) {
        counts->triangles_facing_away++;
        return false;
    }
    counts->triangles_set_up++;
    return true;
}

void
r3d_pipeline_count_triangles(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* clusters, int count,
                             const r3d_pipeline_vertex_t* cs, r3d_pipeline_counts_t* counts, uint16_t* set_up) {
    const r3d_span_target_t screen = r3d_span_target(NULL, NULL, lens->width, 0, lens->height);
    for (int i = 0; i < count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[clusters[i]];
        int reached = 0;
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            const uint16_t* tri = mesh->triangles[t];
            const r3d_pipeline_vertex_t* const v[3] = {&cs[tri[0]], &cs[tri[1]], &cs[tri[2]]};
            reached += count_triangle(v, c->double_sided, &screen, counts) ? 1 : 0;
        }
        if (set_up != NULL) {
            set_up[clusters[i]] = (uint16_t)reached;
        }
    }
}
