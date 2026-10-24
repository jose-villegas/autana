#include "render/view/raster_cull_view.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "core/tune.h"
#include "draw/draw.h"
#include "math/geometry/box3f.h"
#include "math/linear/projectf.h"
#include "math/linear/transformf.h"
#include "render/r3d_span.h"
#include "render/raster_internal.h"
#include "render/view/raster_meshlets.h"

TUNE_OWNER(cullview);
TUNE(cullview, from, RASTER_CULL_FROM_CAMERA, 0, RASTER_CULL_FROM_COUNT - 1);
TUNE(cullview, paint, RASTER_CULL_PAINT_FATES, 0, RASTER_CULL_PAINT_COUNT - 1);
TUNE(cullview, level, 0, 0, R3D_LIT_TREE_DEPTH_MAX);

/* Fates as the view paints them. */
static const uint32_t FATE_RGB[RASTER_FATE_COUNT] = {
    [RASTER_FATE_CULLED] = 0x505050,
    [RASTER_FATE_NOT_SET_UP] = 0x2F6FE0,
    [RASTER_FATE_HIDDEN] = 0xE03C2F,
    [RASTER_FATE_DRAWN] = 0x3CC850,
};
#define FRUSTUM_RGB 0xFFFFFF
/* What a node's hue keeps, out of GFX_COLOR_SCALE_UNIT, under each fate. */
#define KEEP_STEPS  16
static const unsigned FATE_KEEP[RASTER_FATE_COUNT] = {
    [RASTER_FATE_CULLED] = GFX_COLOR_SCALE_UNIT * 3 / KEEP_STEPS,
    [RASTER_FATE_NOT_SET_UP] = GFX_COLOR_SCALE_UNIT * 7 / KEEP_STEPS,
    [RASTER_FATE_HIDDEN] = GFX_COLOR_SCALE_UNIT * 11 / KEEP_STEPS,
    [RASTER_FATE_DRAWN] = GFX_COLOR_SCALE_UNIT,
};

/* A top view frames the bounds' widest half extent with this margin. */
#define TOP_MARGIN     1.1F
/* A chase view sits behind and above the camera by these shares of the
 * bounds' radius, looking at a point as far ahead. */
#define CHASE_BACK     0.25F
#define CHASE_UP       0.15F
/* A spectator's near plane: this share of the bounds' radius, or the camera's. */
#define SPECTATOR_NEAR 0.001F
/* A box's corners, box3f_corner()'s k, each bit an axis. */
#define BOX_CORNERS    8

#define BITS           (sizeof(uint32_t) * CHAR_BIT)

/* Every instance's clusters and triangles, together. */
static void
totals(const raster_t* raster, int* clusters, int* triangles) {
    *clusters = 0;
    *triangles = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        *clusters += raster->instances[i].mesh->cluster_count;
        *triangles += raster->instances[i].mesh->triangle_count;
    }
}

static void
clear(const raster_attachment_t* self, const raster_t* raster, void* pixels, size_t count) {
    (void)self;
    (void)raster;
    memset(pixels, 0, count * sizeof(uint32_t));
}

static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    raster_cull_t* s = self->state;
    s->next_cluster = 0;
    s->next_triangle = 0;
    if (s->spectating) {
        return;
    }
    s->raster = raster;
    s->camera = *view;
    s->from = (raster_cull_from_t)from;
    totals(raster, &s->clusters, &s->triangles);
    assert(s->clusters <= RASTER_CULL_CLUSTERS_MAX && s->triangles <= RASTER_CULL_TRIANGLES_MAX);
    memset(&s->frame, 0, sizeof s->frame);
    memset(s->fate, RASTER_FATE_CULLED, (size_t)s->clusters);
    memset(s->covered, 0, sizeof s->covered);
}

/* The camera's draw tags each pixel with its triangle, a spectator's with
 * its cluster; both count from 1 across the instances. */
static bool
writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    raster_cull_t* s = self->state;
    const r3d_lit_mesh_t* mesh = s->raster->instances[instance].mesh;
    *out = (r3d_span_writer_t){.span = raster_attachment_tag32};
    if (s->spectating) {
        out->step = R3D_SPAN_STEP_CLUSTER;
        out->value = 1U + (uint32_t)s->next_cluster;
    } else {
        out->step = R3D_SPAN_STEP_TRIANGLE;
        out->value = 1U + (uint32_t)s->next_triangle;
    }
    s->writing_cluster = s->next_cluster;
    s->next_cluster += mesh->cluster_count;
    s->next_triangle += mesh->triangle_count;
    return true;
}

static void
drawn(const raster_attachment_t* self, const raster_t* raster, int instance, const r3d_lens_t* lens,
      const uint16_t* clusters, int count, const r3d_pipeline_vertex_t* cs) {
    raster_cull_t* s = self->state;
    if (s->spectating) {
        return;
    }
    const r3d_lit_mesh_t* mesh = raster->instances[instance].mesh;
    const int offset = s->writing_cluster;
    (void)r3d_pipeline_cull_counted(mesh, lens, s->walked, raster_carve(raster).work[0], &s->frame.counts);
    r3d_pipeline_count_triangles(mesh, lens, clusters, count, cs, &s->frame.counts, &s->set_up[offset]);
    for (int i = 0; i < count; i++) {
        const int c = offset + clusters[i];
        s->fate[c] = s->set_up[c] != 0 ? RASTER_FATE_HIDDEN : RASTER_FATE_NOT_SET_UP;
    }
}

static int
attachment_index(const raster_t* raster, const raster_cull_t* s) {
    for (int i = 0; i < raster->attachment_count; i++) {
        if (raster->attachments[i]->state == s) {
            return GFX_ATTACHMENT_FURTHER + i;
        }
    }
    assert(false);
    return GFX_ATTACHMENT_FURTHER;
}

static bool
covered(const raster_cull_t* s, int triangle) {
    return (s->covered[triangle / BITS] >> (triangle % BITS) & 1U) != 0;
}

/* Marks the triangles the camera's picture shows; a set-up cluster owning
 * one of them is drawn. */
static void
tally(raster_cull_t* s, const raster_t* raster) {
    const raster_buffers_t b = raster_carve(raster);
    const int index = attachment_index(raster, s);
    for (int y = 0; y < raster->height; y++) {
        const uint32_t* tag = gfx_render_target_row(&b.picture, index, y);
        for (int x = 0; x < raster->width; x++) {
            if (tag[x] != 0) {
                const uint32_t t = tag[x] - 1U;
                s->covered[t / BITS] |= 1U << (t % BITS);
            }
        }
    }
    int clusters = 0;
    int triangles = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_lit_mesh_t* mesh = raster->instances[i].mesh;
        for (int c = 0; c < mesh->cluster_count; c++) {
            const r3d_lit_cluster_t* cluster = &mesh->clusters[c];
            for (int t = 0; t < cluster->triangle_count; t++) {
                if (covered(s, triangles + cluster->triangle_first + t)) {
                    s->frame.triangles_covering++;
                    s->fate[clusters + c] = RASTER_FATE_DRAWN;
                }
            }
        }
        clusters += mesh->cluster_count;
        triangles += mesh->triangle_count;
    }
    for (int c = 0; c < s->clusters; c++) {
        s->frame.clusters[s->fate[c]]++;
    }
}

/* Each cluster's node at depth `level`, or its leaf where the tree is shallower. */
static void
find_nodes(raster_cull_t* s, const raster_t* raster) {
    int offset = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_lit_mesh_t* mesh = raster->instances[i].mesh;
        uint16_t stack[2][R3D_LIT_WALK_STACK_MAX];
        int top = 0;
        stack[0][top] = 0;
        stack[1][top++] = 0; /* node, then the node standing for it at `level` */
        int depth[R3D_LIT_WALK_STACK_MAX] = {0};
        while (top > 0) {
            top--;
            const r3d_lit_node_t* node = &mesh->nodes[stack[0][top]];
            const uint16_t shown = depth[top] <= level ? stack[0][top] : stack[1][top];
            const int below = depth[top] + 1;
            for (int k = 0; k < node->count; k++) {
                if (node->leaf) {
                    s->node[offset + node->first + k] = shown;
                    continue;
                }
                assert(top < R3D_LIT_WALK_STACK_MAX);
                stack[0][top] = (uint16_t)(node->first + k);
                stack[1][top] = shown;
                depth[top++] = below;
            }
        }
        offset += mesh->cluster_count;
    }
}

/* Every instance's root box, in the world. */
static box3f_t
world_bounds(const raster_t* raster) {
    box3f_t bounds = box3f_empty();
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        const box3f_t local =
            box3f_from_box3s(instance->mesh->nodes[0].bounds, 1.0F / (float)instance->mesh->position_scale);
        for (int k = 0; k < BOX_CORNERS; k++) {
            vec3f_t p = box3f_corner(local, k);
            if (instance->model != NULL) {
                p = mat4f_apply(instance->model, p);
            }
            bounds = box3f_extend(bounds, p);
        }
    }
    return bounds;
}

static render_view_t
spectator_view(const raster_cull_t* s, const raster_t* raster) {
    const render_view_t* camera = &s->camera;
    const box3f_t bounds = world_bounds(raster);
    const vec3f_t centre = box3f_centre(bounds);
    const vec3f_t half = box3f_half_extent(bounds);
    const float radius = sqrtf(vec3f_dot(half, half));
    const float tan_half = render_view_half_fov_short_tan(camera);
    const float near_z = mathf_max(camera->lens.near_z, radius * SPECTATOR_NEAR);
    transformf_t pose;
    if (s->from == RASTER_CULL_FROM_TOP) {
        const float height = (TOP_MARGIN * mathf_max(half.x, half.z) / tan_half) + half.y;
        pose = transformf_looking((vec3f_t){centre.x, centre.y + height, centre.z}, (vec3f_t){0.0F, -1.0F, 0.0F},
                                  (vec3f_t){0.0F, 0.0F, 1.0F});
    } else {
        const vec3f_t ahead = vec3f_add(camera->position, vec3f_scale(camera->forward, CHASE_BACK * radius));
        const vec3f_t eye = vec3f_add(vec3f_sub(camera->position, vec3f_scale(camera->forward, CHASE_BACK * radius)),
                                      (vec3f_t){0.0F, CHASE_UP * radius, 0.0F});
        pose = transformf_looking(eye, vec3f_sub(ahead, eye), (vec3f_t){0.0F, 1.0F, 0.0F});
    }
    render_view_t view = render_view_make(&pose, tan_half, near_z, camera->viewport);
    view.present = camera->present;
    view.small_fill = camera->small_fill;
    return view;
}

bool
raster_cull_second_view(void* state, const raster_t* raster, render_view_t* out) {
    raster_cull_t* s = state;
    tally(s, raster);
    find_nodes(s, raster);
    if (s->from == RASTER_CULL_FROM_CAMERA) {
        return false;
    }
    s->spectator = spectator_view(s, raster);
    s->spectating = true;
    *out = s->spectator;
    return true;
}

/* A cluster's colour: its fate, or its node's hue kept by its fate. */
static gfx_color_t
cluster_color(const raster_cull_t* s, int cluster) {
    const raster_cluster_fate_t fate = (raster_cluster_fate_t)s->fate[cluster];
    if (paint == RASTER_CULL_PAINT_FATES) {
        return GFX_RGB(FATE_RGB[fate]);
    }
    return gfx_color_scale(raster_meshlets_color(1U + s->node[cluster]), FATE_KEEP[fate]);
}

/* The camera's own picture tags triangles: each finds its cluster, the
 * clusters' triangle runs following one another in order. */
static int
cluster_of_triangle(const raster_t* raster, uint32_t triangle) {
    int offset = 0;
    for (int i = 0; i < raster->instance_count; i++) {
        const r3d_lit_mesh_t* mesh = raster->instances[i].mesh;
        if (triangle < (uint32_t)mesh->triangle_count) {
            int lo = 0;
            int hi = mesh->cluster_count - 1;
            while (lo < hi) {
                const int mid = (lo + hi + 1) / 2;
                if (mesh->clusters[mid].triangle_first <= triangle) {
                    lo = mid;
                } else {
                    hi = mid - 1;
                }
            }
            return offset + lo;
        }
        triangle -= (uint32_t)mesh->triangle_count;
        offset += mesh->cluster_count;
    }
    return 0;
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_render_target_t* picture, int index) {
    raster_cull_t* s = self->state;
    for (int y = picture->row0; y < picture->row1; y++) {
        const uint32_t* tag = gfx_render_target_row(picture, index, y);
        gfx_color_t* color = gfx_render_target_color(picture, y);
        for (int x = 0; x < picture->width; x++) {
            if (tag[x] == 0) {
                color[x] = raster->clear;
            } else {
                const int c = s->spectating ? (int)(tag[x] - 1U) : cluster_of_triangle(raster, tag[x] - 1U);
                color[x] = cluster_color(s, c);
            }
        }
    }
    s->spectating = false;
}

raster_attachment_t
raster_cull_view(void* state) {
    return (raster_attachment_t){.bytes_per_pixel = sizeof(uint32_t),
                                 .clear = clear,
                                 .begin = begin,
                                 .writer = writer,
                                 .drawn = drawn,
                                 .show = show,
                                 .state = state};
}

/* One segment in lens space, cut at the near plane, as a line. */
static void
segment(const r3d_lens_t* lens, vec3f_t a, vec3f_t b, gfx_color_t color) {
    if (projectf_clip_near(lens->project.near_z, &a, &b)) {
        const vec2f_t from = projectf_screen(&lens->project, a);
        const vec2f_t to = projectf_screen(&lens->project, b);
        draw_line((int)from.x, (int)from.y, (int)to.x, (int)to.y, color);
    }
}

/* The twelve edges of `box`, in `lens`'s units. */
static void
box_lines(const r3d_lens_t* lens, box3f_t box, gfx_color_t color) {
    vec3f_t corner[BOX_CORNERS];
    for (int k = 0; k < BOX_CORNERS; k++) {
        corner[k] = mat4f_apply(&lens->m, box3f_corner(box, k));
    }
    for (int a = 0; a < BOX_CORNERS; a++) {
        for (int bit = 1; bit < BOX_CORNERS; bit <<= 1) {
            if ((a & bit) == 0) {
                segment(lens, corner[a], corner[a | bit], color);
            }
        }
    }
}

/* The camera's frustum to `depth` world units, its apex at the camera. */
static void
frustum_lines(const r3d_lens_t* lens, const render_view_t* camera, float depth, gfx_color_t color) {
    const float x = depth * (float)camera->viewport.width / (2.0F * camera->lens.fx);
    const float y = depth * (float)camera->viewport.height / (2.0F * camera->lens.fy);
    const vec3f_t far_centre = vec3f_add(camera->position, vec3f_scale(camera->forward, depth));
    vec3f_t corner[4];
    for (int k = 0; k < 4; k++) {
        const vec3f_t side = vec3f_scale(camera->screen_x, (k & 1) ? x : -x);
        const vec3f_t down = vec3f_scale(camera->screen_y, (k & 2) ? y : -y);
        corner[k] = mat4f_apply(&lens->m, vec3f_add(far_centre, vec3f_add(side, down)));
    }
    const vec3f_t apex = mat4f_apply(&lens->m, camera->position);
    static const int ring[4] = {0, 1, 3, 2};
    for (int k = 0; k < 4; k++) {
        segment(lens, apex, corner[k], color);
        segment(lens, corner[ring[k]], corner[ring[(k + 1) % 4]], color);
    }
}

/* The boxes of the nodes at depth cullview.level, and of shallower leaves. */
static void
level_boxes(const r3d_lens_t* lens, const r3d_lit_mesh_t* mesh) {
    uint16_t stack[R3D_LIT_WALK_STACK_MAX];
    uint8_t depth[R3D_LIT_WALK_STACK_MAX];
    int top = 0;
    stack[top] = 0;
    depth[top++] = 0;
    while (top > 0) {
        top--;
        const uint16_t n = stack[top];
        const r3d_lit_node_t* node = &mesh->nodes[n];
        if (depth[top] == level || node->leaf) {
            const gfx_color_t color =
                paint == RASTER_CULL_PAINT_FATES ? GFX_RGB(FRUSTUM_RGB) : raster_meshlets_color(1U + n);
            box_lines(lens, box3f_from_box3s(node->bounds, 1.0F), color);
            continue;
        }
        const uint8_t below = (uint8_t)(depth[top] + 1);
        for (int k = 0; k < node->count; k++) {
            assert(top < R3D_LIT_WALK_STACK_MAX);
            stack[top] = (uint16_t)(node->first + k);
            depth[top++] = below;
        }
    }
}

void
raster_cull_overlay(void* state, int width, int height) {
    const raster_cull_t* s = state;
    if (s->raster == NULL) {
        return;
    }
    const raster_t* raster = s->raster;
    const bool spectated = s->from != RASTER_CULL_FROM_CAMERA;
    render_view_t seen = spectated ? s->spectator : s->camera;
    render_view_refit(&seen, (viewport_t){width, height, seen.viewport.quarter});
    for (int i = 0; level > 0 && i < raster->instance_count; i++) {
        const r3d_instance_t* instance = &raster->instances[i];
        r3d_lens_t lens;
        r3d_lens_init(&lens, &seen, instance->mesh->position_scale);
        if (instance->model != NULL) {
            r3d_lens_place(&lens, instance->model, instance->mesh->position_scale);
        }
        level_boxes(&lens, instance->mesh);
    }
    if (spectated) {
        r3d_lens_t lens;
        r3d_lens_init(&lens, &seen, 1);
        const vec3f_t half = box3f_half_extent(world_bounds(raster));
        const float depth = sqrtf(vec3f_dot(half, half));
        frustum_lines(&lens, &s->camera, depth, GFX_RGB(FRUSTUM_RGB));
    }
}

raster_cull_frame_t
raster_cull_frame(const raster_cull_t* state) {
    return state->frame;
}
