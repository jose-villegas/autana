/*
 * r3d_span_screen: the masked fill of a dithered cluster. Kept out of
 * r3d_span.c, as cold code beside its per-triangle set-up moves that set-up's
 * flash placement and so the frame time of every scene.
 */
#include "render/r3d_span.h"
#include "render/r3d_span_internal.h"

#include <stdbool.h>
#include <stdint.h>

#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"

/* A float plane through three vertices' values, per subpixel from vertex a. */
typedef struct {
    float at, dx, dy;
} screen_plane_t;

static screen_plane_t
screen_plane(const r3d_span_vertex_t* a, const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, float area2, float va,
             float vb, float vc) {
    const float ux = (float)(b->x - a->x);
    const float uy = (float)(b->y - a->y);
    const float wx = (float)(c->x - a->x);
    const float wy = (float)(c->y - a->y);
    return (screen_plane_t){va, (((vb - va) * wy) - ((vc - va) * uy)) / area2,
                            (((vc - va) * ux) - ((vb - va) * wx)) / area2};
}

static inline float
screen_value(const screen_plane_t* p, float x, float y) {
    return p->at + (p->dx * x) + (p->dy * y);
}

/* The bits of one row of the screen mask's tile. */
#define SCREEN_ROW_BITS ((1U << R3D_SPAN_SCREEN_SIDE) - 1U)

/* r3d_span_screen_triangle(): pixel centres tested against the edges as the
 * small fill does, so it covers what the walk would, and each attribute from
 * a float plane. */
void
r3d_span_screen_triangle(const r3d_span_target_t* target, const r3d_span_vertex_t* a, const r3d_span_vertex_t* b,
                         const r3d_span_vertex_t* c, const uint16_t* face, uint16_t mask, bool writing) {
    const bool capped = target->skip_hidden;
    const recti_t box = r3d_span_window_box(target, r3d_span_extent_limits(a, b, c, 0, 0).centres);
    const int32_t area2 = r3d_span_area2(a, b, c);
    if (recti_is_empty(box) || area2 == 0 || r3d_span_stop_after != 0) {
        return;
    }
    const bool positive = area2 > 0;
    const r3d_span_vertex_t* p = positive ? b : c;
    const r3d_span_vertex_t* q = positive ? c : b;
    const r3d_span_edge_t e[] = {r3d_span_edge(a, p), r3d_span_edge(p, q), r3d_span_edge(q, a)};
    const float area = (float)area2;
    const screen_plane_t z = screen_plane(a, b, c, area, a->z, b->z, c->z);
    const screen_plane_t r = screen_plane(a, b, c, area, a->r, b->r, c->r);
    const screen_plane_t g = screen_plane(a, b, c, area, a->g, b->g, c->g);
    const screen_plane_t bl = screen_plane(a, b, c, area, a->b, b->b, c->b);
    const uint16_t cap = capped ? r3d_span_depth_limit(mathf_max(a->z, mathf_max(b->z, c->z))) : R3D_DEPTH_NEAREST;
    for (int y = box.y; y < recti_bottom(box); y++) {
        /* A row of the tile the mask closes whole draws nothing. */
        if (((mask >> R3D_SPAN_SCREEN_BIT(0, y)) & SCREEN_ROW_BITS) == 0) {
            continue;
        }
        const r3d_span_row_t row = r3d_span_row(target, y);
        const float fy = (float)(row.centre - a->y);
        bool entered = false;
        for (int x = box.x, end = recti_right(box); x < end; x++) {
            const int32_t cx = (x << R3D_SUBPIXEL_SHIFT) + R3D_SPAN_HALF_PIXEL;
            if ((r3d_span_side(&e[0], cx, row.centre) | r3d_span_side(&e[1], cx, row.centre)
                 | r3d_span_side(&e[2], cx, row.centre))
                < 0) {
                if (entered) {
                    break; /* convex: past its right edge, the row is done */
                }
                continue;
            }
            entered = true;
            if (((mask >> R3D_SPAN_SCREEN_BIT(x, y)) & 1U) == 0) {
                continue;
            }
            const float fx = (float)(cx - a->x);
            const uint16_t depth = (uint16_t)mathi_min(
                (int)(mathf_clamp(screen_value(&z, fx, fy), 0.0F, 1.0F) * (float)R3D_DEPTH_NEAREST), cap);
            if (writing) {
                r3d_span_write(target, y, x, x, (int32_t)depth << R3D_DEPTH_SHIFT, 0);
            } else if (depth > row.depth[x]) {
                row.depth[x] = depth;
                row.color[x] = face != NULL ? *face
                                            : r3d_span_pack_rgb(screen_value(&r, fx, fy), screen_value(&g, fx, fy),
                                                                screen_value(&bl, fx, fy));
            }
        }
    }
}
