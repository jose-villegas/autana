#include "render/view/raster_cull_view.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "core/tune.h"
#include "image/pixel/rgb565.h"
#include "math/geometry/box3f.h"
#include "math/linear/transformf.h"
#include "math/projection/projectf.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_span.h"
#include "render/raster_internal.h"
#include "render/view/r3d_pipeline_count.h"
#include "render/view/raster_lines.h"
#include "render/view/raster_meshlets.h"

TUNE_OWNER(cullview);
TUNE(cullview, from, RASTER_CULL_FROM_CAMERA, 0, RASTER_CULL_FROM_COUNT - 1);
TUNE(cullview, paint, RASTER_CULL_PAINT_SHADED, 0, RASTER_CULL_PAINT_COUNT - 1);
TUNE(cullview, level, 0, 0, R3D_LIT_TREE_DEPTH_MAX);

/* Fates as the view paints them. */
static const uint32_t FATE_RGB[RASTER_FATE_COUNT] = {
    [RASTER_FATE_CULLED] = 0x505050,
    [RASTER_FATE_NOT_SET_UP] = 0x2F6FE0,
    [RASTER_FATE_HIDDEN] = 0xE03C2F,
    [RASTER_FATE_DRAWN] = 0x3CC850,
};
#define FRUSTUM_RGB 0xFFFFFF
/* A culled cluster is a screen door: one pixel of each square this many on a
 * side keeps its shading, the rest show the clear colour. */
#define CULLED_DOOR 4
/* Its border is dashed: this many pixels on, as many off, along the edge. */
#define CULLED_DASH 2
/* What a node's hue keeps, out of RGB565_SCALE_UNIT, under each fate. */
#define KEEP_STEPS  16
static const unsigned FATE_KEEP[RASTER_FATE_COUNT] = {
    [RASTER_FATE_CULLED] = RGB565_SCALE_UNIT * 3 / KEEP_STEPS,
    [RASTER_FATE_NOT_SET_UP] = RGB565_SCALE_UNIT * 7 / KEEP_STEPS,
    [RASTER_FATE_HIDDEN] = RGB565_SCALE_UNIT * 11 / KEEP_STEPS,
    [RASTER_FATE_DRAWN] = RGB565_SCALE_UNIT,
};

/* A top view frames the bounds' widest half extent with this margin. */
#define TOP_MARGIN     1.1F
/* A chase view sits behind and above the camera by these shares of the
 * bounds' radius, looking at a point as far ahead. */
#define CHASE_BACK     0.25F
#define CHASE_UP       0.15F
/* A spectator's near plane: this share of the bounds' radius, or the camera's. */
#define SPECTATOR_NEAR 0.001F

#define BITS           (sizeof(uint32_t) * CHAR_BIT)

/* Each instance's drawn clusters into s->drawn, the level each instance
 * drew this frame (both levels of a group it dithers), and how many
 * clusters and triangles that is. */
static void
list_drawn(raster_cull_t* s, const raster_t* raster) {
    int at = 0;
    s->clusters = 0;
    s->triangles = 0;
    for (int i = 0; i < raster_drawn_count(raster); i++) {
        const r3d_instance_t* instance = raster_drawn_at(raster, i);
        const r3d_lit_mesh_t* mesh = instance->mesh;
        assert(at < RASTER_CULL_CLUSTERS_MAX);
        const int head = at++;
        const int count = r3d_lod_list_drawn(mesh, instance->lod_coarse, raster_lod_dithering(raster, instance),
                                             &s->drawn[at], RASTER_CULL_CLUSTERS_MAX - at);
        for (int k = 0; k < count; k++) {
            s->triangles += mesh->clusters[s->drawn[at + k]].triangle_count;
        }
        at += count;
        s->drawn[head] = (uint16_t)count;
        s->clusters += count;
    }
    assert(s->triangles <= RASTER_CULL_TRIANGLES_MAX);
}

/* Where mesh cluster `c` sits in the drawn list at drawn[at], or -1 when the
 * level drawn leaves it out. */
static int
index_in(const raster_cull_t* s, int at, int c) {
    const uint16_t* list = &s->drawn[at + 1];
    int lo = 0;
    int hi = s->drawn[at] - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (list[mid] == c) {
            return mid;
        }
        if (list[mid] < c) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

/* An instance's place in the drawn lists: its list at drawn[at], and the
 * slot and the pair of triangle_first it starts at. */
typedef struct {
    int at, base, pair;
} drawn_at_t;

/* The next instance's place. */
static void
walk_next(const raster_cull_t* s, drawn_at_t* w) {
    const int count = s->drawn[w->at];
    w->base += count;
    w->pair += (count + 1) / 2;
    w->at += 1 + count;
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
    memset(&s->frame, 0, sizeof s->frame);
    memset(s->covered, 0, sizeof s->covered);
}

/* The camera's draw tags each pixel with its triangle, a spectator's with
 * its cluster; both count from 1 across the instances. */
static bool
writer(const raster_attachment_t* self, int instance, r3d_span_writer_t* out) {
    raster_cull_t* s = self->state;
    const r3d_lit_mesh_t* mesh = raster_drawn_at(s->raster, instance)->mesh;
    *out = (r3d_span_writer_t){.span = raster_attachment_tag};
    if (s->spectating) {
        out->step = R3D_SPAN_STEP_CLUSTER;
        out->value = 1U + (uint32_t)s->next_cluster;
    } else {
        out->step = R3D_SPAN_STEP_TRIANGLE;
        out->value = 1U + (uint32_t)s->next_triangle;
    }
    s->next_cluster += mesh->cluster_count;
    s->next_triangle += mesh->triangle_count;
    return true;
}

/* Without culling, every drawn cluster, the `count` of `drawn`, is kept: counted as the cull counts them. */
static void
keep_drawn(const r3d_lit_mesh_t* mesh, const uint16_t* drawn, int count, r3d_pipeline_counts_t* counts) {
    for (int k = 0; k < count; k++) {
        counts->triangles_submitted += mesh->clusters[drawn[k]].triangle_count;
    }
    counts->clusters_tested += count;
    counts->clusters_kept += count;
    counts->clusters_coarse += r3d_pipeline_count_coarse(mesh, drawn, count);
}

/* Hundredths: the unit the stacked-blend bound is counted in. */
#define RASTER_CULL_CENTI 100.0F

/* Kept cluster `cluster`'s vertices: transformed, and blended or stacked by `morph`. */
static void
count_vertices(r3d_pipeline_counts_t* counts, const r3d_lod_morph_t* morph, const r3d_lit_mesh_t* mesh, int cluster) {
    const int vertices = mesh->clusters[cluster].vertex_count;
    counts->vertices_transformed += vertices;
    const bool blended =
        r3d_lod_morph_weight(morph, mesh, cluster) != 0 || r3d_lod_dither_band(morph, mesh, cluster) != 0;
    counts->vertices_blended += blended ? vertices : 0;
    const float stacked = r3d_pipeline_count_stacked_px(morph, mesh, cluster);
    if (stacked > 0.0F) {
        counts->vertices_stacked += vertices;
        counts->stacked_centipixels = mathi_max(counts->stacked_centipixels, (int)(stacked * RASTER_CULL_CENTI));
    }
}

/* The fates of the instance at `w`'s `count` kept clusters: hidden where set
 * up, else not set up. */
static void
mark_set_up(raster_cull_t* s, const drawn_at_t* w, const uint16_t* kept, int count) {
    for (int k = 0; k < count; k++) {
        const int j = index_in(s, w->at, kept[k]);
        assert(j >= 0);
        s->fate[w->base + j] = s->set_up[k] != 0 ? RASTER_FATE_HIDDEN : RASTER_FATE_NOT_SET_UP;
    }
}

/* After the camera's draw, which picked each instance's level, its drawn
 * clusters are listed, and its scratch lists and vertices are free: each
 * instance is culled, counted and transformed again into them, through the
 * lens the draw used, and its kept clusters are not set up or hidden. The
 * walk starts at each root, so its node counts include instances the frame's
 * world-box test dropped; the fates are the same. */
static void
count_frame(raster_cull_t* s, const raster_t* raster) {
    list_drawn(s, raster);
    memset(s->fate, RASTER_FATE_CULLED, (size_t)s->clusters);
    const raster_buffers_t b = raster_carve(raster);
    const bool culls = raster_settings(raster).cull; /* whether the frame culled at all */
    drawn_at_t w = {0};
    for (int i = 0; i < raster_drawn_count(raster); i++) {
        const r3d_instance_t* instance = raster_drawn_at(raster, i);
        r3d_lit_mesh_t counted_mesh = *instance->mesh;
        const r3d_batch_draw_t* copy = raster_drawn_copy(raster, i);
        if (copy != NULL) {
            r3d_batch_copy_mesh(copy->batch, instance->mesh, copy->copy, &counted_mesh);
        }
        const r3d_lit_mesh_t* mesh = &counted_mesh;
        r3d_lens_t lens;
        raster_lens(raster, &s->camera, mesh->position_scale, &lens);
        raster_drawn_place(raster, i, &lens);
        const uint16_t* kept = &s->drawn[w.at + 1];
        int count = s->drawn[w.at];
        if (culls) {
            kept = b.culled;
            count = r3d_pipeline_cull_counted(mesh, &lens, instance->lod_coarse, raster_lod_dithering(raster, instance),
                                              b.culled, &s->walk, &s->frame.counts);
        } else {
            keep_drawn(mesh, kept, count, &s->frame.counts);
        }
        r3d_lod_morph_t morph;
        raster_lod_morph(raster, &s->camera, instance, &lens, &morph);
        r3d_pipeline_transform_morphed(mesh, &lens, &morph, kept, count, b.cs, NULL, false);
        for (int k = 0; k < count; k++) {
            count_vertices(&s->frame.counts, &morph, mesh, kept[k]);
        }
        r3d_pipeline_count_triangles(mesh, &lens, kept, count, b.cs, &s->frame.counts, s->set_up);
        mark_set_up(s, &w, kept, count);
        walk_next(s, &w);
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

/* The cluster of `mesh` owning triangle `triangle`, the clusters' triangle
 * runs following one another in order. */
static int
cluster_of(const r3d_lit_mesh_t* mesh, uint32_t triangle) {
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
    return lo;
}

/* The instance a tag last fell in: its IDs first to end - 1, its mesh and
 * its place in the drawn lists. Neighbouring pixels mostly share one, so a
 * picture's walk keeps a hint and goes through the instances only on a miss. */
typedef struct {
    uint32_t first, end;
    const r3d_lit_mesh_t* mesh;
    drawn_at_t at;
} tag_hint_t;

/* Where a tag falls: its instance's mesh and place in the drawn lists, and
 * the ID within the mesh. Tags count clusters, or triangles, across the
 * instances' whole meshes, from 1; 0 is none. */
static bool
place_tag(const raster_cull_t* s, const raster_t* raster, uint32_t tag, bool clusters, tag_hint_t* hint, uint32_t* id) {
    if (tag == 0) {
        return false;
    }
    const uint32_t wanted = tag - 1U;
    if (wanted < hint->first || wanted >= hint->end) {
        drawn_at_t w = {0};
        uint32_t first = 0;
        int i = 0;
        for (; i < raster_drawn_count(raster); i++) {
            const r3d_lit_mesh_t* mesh = raster_drawn_at(raster, i)->mesh;
            const uint32_t span = (uint32_t)(clusters ? mesh->cluster_count : mesh->triangle_count);
            if (wanted < first + span) {
                *hint = (tag_hint_t){first, first + span, mesh, w};
                break;
            }
            first += span;
            walk_next(s, &w);
        }
        if (i == raster_drawn_count(raster)) {
            return false;
        }
    }
    *id = wanted - hint->first;
    return true;
}

static bool
covered(const raster_cull_t* s, uint32_t triangle) {
    return (s->covered[triangle / BITS] >> (triangle % BITS) & 1U) != 0;
}

/* Each drawn pair's first drawn triangle, the drawn triangles counted across
 * the instances in slot order: one per pair, so they fit node's room. */
static void
pair_triangles(raster_cull_t* s, const raster_t* raster) {
    drawn_at_t w = {0};
    uint32_t first = 0;
    for (int i = 0; i < raster_drawn_count(raster); i++) {
        const r3d_lit_mesh_t* mesh = raster_drawn_at(raster, i)->mesh;
        assert(w.pair + ((s->drawn[w.at] + 1) / 2) <= RASTER_CULL_CLUSTERS_MAX / 2);
        for (int j = 0; j < s->drawn[w.at]; j++) {
            if (j % 2 == 0) {
                s->triangle_first[w.pair + (j / 2)] = first;
            }
            first += mesh->clusters[s->drawn[w.at + 1 + j]].triangle_count;
        }
        walk_next(s, &w);
    }
}

/* Marks the drawn triangles the camera's picture shows, each by its place
 * among the drawn triangles. */
static void
mark_covered(raster_cull_t* s, const raster_t* raster) {
    pair_triangles(s, raster);
    const raster_buffers_t b = raster_carve(raster);
    const int index = attachment_index(raster, s);
    tag_hint_t hint = {0};
    for (int y = 0; y < raster->height; y++) {
        const uint32_t* tag = gfx_target_row(&b.picture, index, y);
        for (int x = 0; x < raster->width; x++) {
            uint32_t t;
            if (!place_tag(s, raster, tag[x], false, &hint, &t)) {
                continue;
            }
            const r3d_lit_mesh_t* mesh = hint.mesh;
            const drawn_at_t w = hint.at;
            const int c = cluster_of(mesh, t);
            const int j = index_in(s, w.at, c);
            assert(j >= 0);
            uint32_t first = s->triangle_first[w.pair + (j / 2)];
            if (j % 2 != 0) {
                first += mesh->clusters[s->drawn[w.at + j]].triangle_count;
            }
            const uint32_t drawn = first + t - mesh->clusters[c].triangle_first;
            s->covered[drawn / BITS] |= 1U << (drawn % BITS);
        }
    }
}

/* A cluster owning a covered triangle is drawn; then the fates are counted. */
static void
tally(raster_cull_t* s, const raster_t* raster) {
    mark_covered(s, raster);
    drawn_at_t w = {0};
    uint32_t first = 0;
    for (int i = 0; i < raster_drawn_count(raster); i++) {
        const r3d_lit_mesh_t* mesh = raster_drawn_at(raster, i)->mesh;
        for (int j = 0; j < s->drawn[w.at]; j++) {
            const r3d_lit_cluster_t* cluster = &mesh->clusters[s->drawn[w.at + 1 + j]];
            for (int t = 0; t < cluster->triangle_count; t++) {
                if (covered(s, first + (uint32_t)t)) {
                    s->frame.triangles_covering++;
                    s->fate[w.base + j] = RASTER_FATE_DRAWN;
                }
            }
            first += cluster->triangle_count;
        }
        walk_next(s, &w);
    }
    for (int c = 0; c < s->clusters; c++) {
        s->frame.clusters[s->fate[c]]++;
    }
}

/* `shown` for each cluster under `leaf` the instance at `w` drew. */
static void
name_leaf(raster_cull_t* s, const r3d_lit_node_t* leaf, drawn_at_t w, uint16_t shown) {
    for (int k = 0; k < leaf->count; k++) {
        const int j = index_in(s, w.at, leaf->first + k);
        if (j >= 0) {
            s->node[w.base + j] = shown;
        }
    }
}

/* The node at depth `level` (or the leaf, where the tree is shallower) of each
 * cluster `mesh` drew, the instance's clusters starting at slot `w.base`. */
static void
find_nodes_of(raster_cull_t* s, const r3d_lit_mesh_t* mesh, drawn_at_t w) {
    uint16_t* node_of = s->walk_node;
    uint16_t* shown_of = s->walk_shown; /* the node standing for it at `level` */
    uint8_t* depth = s->walk_depth;
    int top = 0;
    node_of[top] = 0;
    shown_of[top] = 0;
    depth[top++] = 0;
    while (top > 0) {
        top--;
        const r3d_lit_node_t* node = &mesh->nodes[node_of[top]];
        const uint16_t shown = depth[top] <= level ? node_of[top] : shown_of[top];
        if (node->leaf) {
            name_leaf(s, node, w, shown);
            continue;
        }
        const uint8_t below = (uint8_t)(depth[top] + 1);
        for (int k = 0; k < node->count; k++) {
            assert(top < R3D_LIT_WALK_STACK_MAX);
            node_of[top] = (uint16_t)(node->first + k);
            shown_of[top] = shown;
            depth[top++] = below;
        }
    }
}

/* Each drawn cluster's node at depth `level`, or its leaf where the tree is shallower. */
static void
find_nodes(raster_cull_t* s, const raster_t* raster) {
    drawn_at_t w = {0};
    for (int i = 0; i < raster_drawn_count(raster); i++) {
        find_nodes_of(s, raster_drawn_at(raster, i)->mesh, w);
        walk_next(s, &w);
    }
}

/* Every instance's root box, in the world. */
static box3f_t
world_bounds(const raster_t* raster) {
    box3f_t bounds = box3f_empty();
    for (int i = 0; i < raster_drawn_count(raster); i++) {
        const r3d_instance_t* instance = raster_drawn_at(raster, i);
        mat4f_t placed;
        const mat4f_t* model = instance->model;
        const r3d_batch_draw_t* copy = raster_drawn_copy(raster, i);
        if (copy != NULL) {
            placed = r3d_batch_copy_model(copy->batch, copy->copy);
            placed = model != NULL ? mat4f_mul_affine(*model, placed) : placed;
            model = &placed;
        }
        bounds = box3f_union(bounds, r3d_lit_mesh_world_bounds(instance->mesh, model));
    }
    return bounds;
}

/* From `observer` when given, else from cullview.from's pose, written to
 * `pose`; the lens is the camera's either way. */
static render_view_t
spectator_view(const raster_cull_t* s, const raster_t* raster, const transformf_t* observer, transformf_t* pose) {
    const render_view_t* camera = &s->camera;
    const box3f_t bounds = world_bounds(raster);
    const vec3f_t centre = box3f_centre(bounds);
    const vec3f_t half = box3f_half_extent(bounds);
    const float radius = sqrtf(vec3f_dot(half, half));
    const float tan_half = render_view_half_fov_short_tan(camera);
    const float near_z = mathf_max(camera->lens.near_z, radius * SPECTATOR_NEAR);
    if (observer != NULL) {
        *pose = *observer;
    } else if (s->from == RASTER_CULL_FROM_TOP) {
        const float height = (TOP_MARGIN * mathf_max(half.x, half.z) / tan_half) + half.y;
        *pose = transformf_looking((vec3f_t){centre.x, centre.y + height, centre.z}, vec3f_down(), vec3f_unit_z());
    } else {
        const vec3f_t ahead = vec3f_add(camera->position, vec3f_scale(camera->forward, CHASE_BACK * radius));
        const vec3f_t eye = vec3f_add(vec3f_sub(camera->position, vec3f_scale(camera->forward, CHASE_BACK * radius)),
                                      (vec3f_t){0.0F, CHASE_UP * radius, 0.0F});
        *pose = transformf_looking(eye, vec3f_sub(ahead, eye), vec3f_unit_y());
    }
    render_view_t view = render_view_make(pose, tan_half, near_z, camera->viewport);
    view.present = camera->present;
    view.small_fill = camera->small_fill;
    return view;
}

/* The census list of each instance: its culled drawn clusters, or its kept
 * ones; a level not drawn is in neither. The list was never fitted, so the
 * spectator encodes depth with its own clip plane. */
static void
list_clusters(const raster_cull_t* s, const raster_t* raster, bool culled) {
    const raster_buffers_t buffers = raster_carve(raster);
    *buffers.depth_near = s->spectator.lens.near_z;
    uint16_t* list = buffers.culled;
    drawn_at_t w = {0};
    for (int i = 0; i < raster_drawn_count(raster); i++) {
        const r3d_lit_mesh_t* mesh = raster_drawn_at(raster, i)->mesh;
        int count = 0;
        for (int j = 0; j < s->drawn[w.at]; j++) {
            if ((s->fate[w.base + j] == RASTER_FATE_CULLED) == culled) {
                list[1 + count++] = s->drawn[w.at + 1 + j];
            }
        }
        list[0] = (uint16_t)count;
        list += 1 + mesh->cluster_count;
        walk_next(s, &w);
    }
}

static void paint_picture(raster_cull_t* s, const raster_t* raster, const gfx_target_t* picture, int index);

/* The culled clusters' picture painted and kept with its depth, for the
 * kept clusters' to be layered on. */
static void
keep_under(raster_cull_t* s, const raster_t* raster) {
    const raster_buffers_t b = raster_carve(raster);
    const int index = attachment_index(raster, s);
    paint_picture(s, raster, &b.picture, index);
    assert(raster->width * raster->height <= RASTER_CULL_PIXELS_MAX);
    for (int y = 0; y < raster->height; y++) {
        const gfx_color_t* color = gfx_target_color(&b.picture, y);
        const uint16_t* depth = gfx_target_depth(&b.picture, y);
        for (int x = 0; x < raster->width; x++) {
            const int p = (y * raster->width) + x;
            s->under_color[p] = color[x];
            s->under_depth[p] = color[x] == raster->clear ? R3D_DEPTH_EMPTY : depth[x];
        }
    }
}

bool
raster_cull_second_view(void* state, const raster_t* raster, const transformf_t* observer, render_view_t* out,
                        bool* listed) {
    raster_cull_t* s = state;
    switch ((raster_cull_pass_t)s->pass) {
        case RASTER_CULL_PASS_CAMERA:
            count_frame(s, raster);
            tally(s, raster);
            find_nodes(s, raster);
            s->spectated = observer != NULL || s->from != RASTER_CULL_FROM_CAMERA;
            if (!s->spectated) {
                return false;
            }
            s->spectator = spectator_view(s, raster, observer, &s->spectator_pose);
            s->spectating = true;
            s->pass = RASTER_CULL_PASS_CULLED;
            list_clusters(s, raster, true);
            break;
        case RASTER_CULL_PASS_CULLED:
            keep_under(s, raster);
            s->pass = RASTER_CULL_PASS_KEPT;
            list_clusters(s, raster, false);
            break;
        case RASTER_CULL_PASS_KEPT: return false;
    }
    *out = s->spectator;
    *listed = true;
    return true;
}

/* A cluster's colour: its fate, or its node's hue kept by its fate. */
static gfx_color_t
cluster_color(const raster_cull_t* s, int cluster) {
    const raster_cluster_fate_t fate = (raster_cluster_fate_t)s->fate[cluster];
    if (paint == RASTER_CULL_PAINT_FATES) {
        return RGB565BE(FATE_RGB[fate]);
    }
    return rgb565be_scale(raster_meshlets_color(1U + s->node[cluster]), FATE_KEEP[fate]);
}

/* The slot of the cluster a tag names, or -1 for none: a spectator's tags
 * are clusters, the camera's triangles, each finding its cluster. */
static int
tagged_cluster(const raster_cull_t* s, const raster_t* raster, uint32_t tag, tag_hint_t* hint) {
    uint32_t id;
    if (!place_tag(s, raster, tag, s->spectating, hint, &id)) {
        return -1;
    }
    const int j = index_in(s, hint->at.at, s->spectating ? (int)id : cluster_of(hint->mesh, id));
    return j < 0 ? -1 : hint->at.base + j;
}

/* One shaded pixel of cluster `c` (-1 for none) at (x, y), on its border or inside. */
static gfx_color_t
shaded_pixel(const raster_cull_t* s, gfx_color_t clear, gfx_color_t drawn, int c, bool border, int x, int y) {
    if (c < 0) {
        return clear;
    }
    const raster_cluster_fate_t fate = (raster_cluster_fate_t)s->fate[c];
    if (fate == RASTER_FATE_CULLED) {
        if (border) {
            return ((x + y) / CULLED_DASH) % 2 == 0 ? RGB565BE(FATE_RGB[fate]) : clear;
        }
        return x % CULLED_DOOR == 0 && y % CULLED_DOOR == 0 ? drawn : clear;
    }
    return border ? RGB565BE(FATE_RGB[fate]) : drawn;
}

/* Shaded: the baked colours as drawn, culled clusters as a sparse screen
 * door, and every pixel where the cluster changes, right or below, in its
 * fate's colour: the clusters' borders, a culled one's dashed. */
static void
show_shaded(raster_cull_t* s, const raster_t* raster, const gfx_target_t* picture, int index) {
    tag_hint_t hint = {0};
    tag_hint_t below_hint = {0};
    for (int y = picture->first_row; y < gfx_target_end_row(picture); y++) {
        const uint32_t* tag = gfx_target_row(picture, index, y);
        const uint32_t* below = y + 1 < gfx_target_end_row(picture) ? gfx_target_row(picture, index, y + 1) : NULL;
        gfx_color_t* color = gfx_target_color(picture, y);
        int right = tagged_cluster(s, raster, tag[0], &hint);
        for (int x = 0; x < picture->attachment[0].width; x++) {
            const int c = right;
            right = x + 1 < picture->attachment[0].width ? tagged_cluster(s, raster, tag[x + 1], &hint) : c;
            const int under = below != NULL ? tagged_cluster(s, raster, below[x], &below_hint) : c;
            color[x] = shaded_pixel(s, raster->clear, color[x], c, c != right || c != under, x, y);
        }
    }
}

/* The picture's colour from its cluster map, in the paint chosen. */
static void
paint_picture(raster_cull_t* s, const raster_t* raster, const gfx_target_t* picture, int index) {
    if (paint == RASTER_CULL_PAINT_SHADED) {
        show_shaded(s, raster, picture, index);
        return;
    }
    tag_hint_t hint = {0};
    for (int y = picture->first_row; y < gfx_target_end_row(picture); y++) {
        const uint32_t* tag = gfx_target_row(picture, index, y);
        gfx_color_t* color = gfx_target_color(picture, y);
        for (int x = 0; x < picture->attachment[0].width; x++) {
            const int c = tagged_cluster(s, raster, tag[x], &hint);
            color[x] = c < 0 ? raster->clear : cluster_color(s, c);
        }
    }
}

/* The culled clusters' kept pixels over the kept clusters' picture: over
 * the background, and over kept geometry they are nearer than. Each takes
 * its depth too, which the upscale reads to tell drawn from clear. */
static void
layer_under(const raster_cull_t* s, const gfx_target_t* picture, int index) {
    for (int y = picture->first_row; y < gfx_target_end_row(picture); y++) {
        gfx_color_t* color = gfx_target_color(picture, y);
        uint16_t* depth = gfx_target_depth(picture, y);
        const uint32_t* tag = gfx_target_row(picture, index, y);
        for (int x = 0; x < picture->attachment[0].width; x++) {
            const int p = (y * picture->attachment[0].width) + x;
            if (s->under_depth[p] != R3D_DEPTH_EMPTY && (tag[x] == 0 || s->under_depth[p] > depth[x])) {
                color[x] = s->under_color[p];
                depth[x] = s->under_depth[p];
            }
        }
    }
}

static void
show(const raster_attachment_t* self, const raster_t* raster, const gfx_target_t* picture, int index) {
    raster_cull_t* s = self->state;
    paint_picture(s, raster, picture, index);
    if (s->pass == RASTER_CULL_PASS_KEPT) {
        layer_under(s, picture, index);
    }
    s->spectating = false;
    s->pass = RASTER_CULL_PASS_CAMERA;
}

raster_attachment_t
raster_cull_view(void* state) {
    return (raster_attachment_t){
        .format = IMAGE_U32, .clear = clear, .begin = begin, .writer = writer, .show = show, .state = state};
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
        raster_lines_segment(lens, NULL, apex, corner[k], color);
        raster_lines_segment(lens, NULL, corner[ring[k]], corner[ring[(k + 1) % 4]], color);
    }
}

/* The boxes of the nodes at depth cullview.level, and of shallower leaves. */
static void
level_boxes(raster_cull_t* s, const r3d_lens_t* lens, const r3d_lit_mesh_t* mesh) {
    uint16_t* stack = s->walk_node;
    uint8_t* depth = s->walk_depth;
    int top = 0;
    stack[top] = 0;
    depth[top++] = 0;
    while (top > 0) {
        top--;
        const uint16_t n = stack[top];
        const r3d_lit_node_t* node = &mesh->nodes[n];
        if (depth[top] == level || node->leaf) {
            const gfx_color_t color =
                paint != RASTER_CULL_PAINT_NODES ? RGB565BE(FRUSTUM_RGB) : raster_meshlets_color(1U + n);
            raster_lines_box(lens, NULL, box3f_from_box3s(node->bounds, 1.0F), color);
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
    raster_cull_t* s = state;
    if (s->raster == NULL) {
        return;
    }
    const raster_t* raster = s->raster;
    render_view_t seen = s->spectated ? s->spectator : s->camera;
    render_view_refit(&seen, (viewport_t){width, height, seen.viewport.quarter});
    for (int i = 0; level > 0 && i < raster_drawn_count(raster); i++) {
        const r3d_instance_t* instance = raster_drawn_at(raster, i);
        r3d_lens_t lens;
        r3d_lens_init(&lens, &seen, instance->mesh->position_scale);
        raster_drawn_place(raster, i, &lens);
        level_boxes(s, &lens, instance->mesh);
    }
    if (s->spectated) {
        r3d_lens_t lens;
        r3d_lens_init(&lens, &seen, 1);
        const vec3f_t half = box3f_half_extent(world_bounds(raster));
        const float depth = sqrtf(vec3f_dot(half, half));
        frustum_lines(&lens, &s->camera, depth, RGB565BE(FRUSTUM_RGB));
    }
}

void
raster_cull_frame_compose(raster_cull_frame_t* frame, const raster_t* raster, bool shaded_half_picture) {
    const upscale_t* scale = &raster->upscale;
    const resolution_step_t output = shaded_half_picture
                                         ? raster_compose_size(raster)
                                         : (resolution_step_t){scale->destination_width, scale->destination_height};
    const upscale_path_t path = output.width == scale->destination_width && output.height == scale->destination_height
                                    ? scale->path
                                    : upscale_path_for(raster->width, raster->height, output.width, output.height);
    frame->transform_batches = raster->scratch != NULL ? raster_batch_count(raster) : 0;
    frame->render_width = raster->width;
    frame->render_height = raster->height;
    frame->output_width = output.width;
    frame->output_height = output.height;
    frame->upscale_path = path;
    frame->upscale_surface =
        path == UPSCALE_KEPT && scale->filter == UPSCALE_FILTER_SURFACE && raster->present != CAMERA_PRESENT_SCANLINE;
}

void
raster_cull_frame(const raster_cull_t* state, raster_cull_frame_t* frame) {
    *frame = state->frame;
    if (state->raster != NULL && state->raster->width > 0 && state->raster->height > 0) {
        const upscale_t* scale = &state->raster->upscale;
        if (scale->destination_width > 0 && scale->destination_height > 0) {
            raster_cull_frame_compose(frame, state->raster, false);
        }
        frame->quarter = state->camera.viewport.quarter;
    }
}
