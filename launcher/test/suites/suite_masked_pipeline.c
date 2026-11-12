/*
 * Portable suite: r3d_pipeline draws a lit mesh's masked cluster through its atlas. The mesh is built inside the
 * test: an opaque cluster, then a masked one whose vertices come after the opaque ones and whose u, v differ
 * per vertex. Each masked pixel is checked against the plane the vertices' u, v make over the screen.
 */

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "render_view_fixture.h"
#include "suites.h"
#include "unity.h"

#include "display/viewport.h"
#include "image/pixel/rgb565.h"
#include "render/lmsk_format.h"
#include "render/r3d_pipeline.h"
#include "render/r3d_span_internal.h"

#define SCREEN_W                     64
#define SCREEN_H                     48
#define SCREEN_PIXELS                (SCREEN_W * SCREEN_H)
#define QUAD_CORNERS                 4
#define QUAD_TRIANGLES               2
#define CLUSTERS                     2
#define MESH_VERTICES                (CLUSTERS * QUAD_CORNERS)
#define MESH_TRIANGLES               (CLUSTERS * QUAD_TRIANGLES)
#define MASK_VERTEX_FIRST            QUAD_CORNERS
/* The mask's u, v table has room beyond its vertices, zeroed, so an index that forgets mask_vertex_first reads
 * zeros, not whatever follows. */
#define MASK_UV_SLOTS                MESH_VERTICES
#define ATLAS_ROWS                   32
#define ATLAS_ROW_HASH               UINT32_C(0x9E3779B1) /* each row a different, mixed word */
#define UV_UNIT                      (1 << LMSK_UV_FRACTION_BITS)
#define DEPTH_KINDS                  3 /* before a draw: nearer than the quad, the quad's own, empty */
#define PIXEL_CENTRE                 0.5F
/* A sample closer to a texel boundary than this is not tested: a draw's rounding could tip it. */
#define EDGE_MARGIN                  (1.0F / 32.0F)

#define OPAQUE_COLOR                 RGB565BE(0x00FF00)
#define MASKED_COLOR                 RGB565BE(0xFF0000)

/* Pixels the quads span: the opaque one left of the middle, the masked one right of it, 24 rows. */
#define OPAQUE_X0                    2
#define OPAQUE_X1                    26
#define MASKED_X0                    38
#define MASKED_X1                    62
#define QUAD_Y0                      12
#define QUAD_Y1                      36
/* The quads' corners, in ticks, 400 from a camera whose 48 pixels per unit of tangent make 100 ticks 12 pixels. */
#define CAMERA_BACK                  (-400)
#define PIXELS_PER_100_TICKS         12
#define TICKS_OF(pixels_from_centre) ((pixels_from_centre) * 100 / PIXELS_PER_100_TICKS)
#define CENTRE_X                     (SCREEN_W / 2)
#define CENTRE_Y                     (SCREEN_H / 2)

typedef struct {
    int16_t positions[MESH_VERTICES][3];
    uint8_t colors[MESH_VERTICES][3];
    uint16_t triangles[MESH_TRIANGLES][3];
    uint16_t face_colors[MESH_TRIANGLES];
    r3d_lit_cluster_t clusters[CLUSTERS];
    r3d_lit_node_t nodes[1];
    uint16_t uvs[MASK_UV_SLOTS][2];
    uint32_t atlas[ATLAS_ROWS];
    r3d_lit_mesh_t mesh;
} scene_mesh_t;

typedef struct {
    uint16_t color[SCREEN_PIXELS];
    uint16_t depth[SCREEN_PIXELS];
    uint16_t hits[SCREEN_PIXELS]; /* times a writer was handed the pixel and found it had won it */
    uint16_t raw[SCREEN_PIXELS];  /* times a writer was handed the pixel at all */
    r3d_pipeline_vertex_t cs[MESH_VERTICES];
} screen_t;

#define MESHES_MAX 2

static scene_mesh_t* meshes_owned[MESHES_MAX];
static screen_t* screens_owned[3];
static r3d_pipeline_work_t* work_owned;
static screen_t* sink;

static void
cleanup(void) {
    for (size_t i = 0; i < MESHES_MAX; i++) {
        free(meshes_owned[i]);
        meshes_owned[i] = NULL;
    }
    for (size_t i = 0; i < sizeof screens_owned / sizeof screens_owned[0]; i++) {
        free(screens_owned[i]);
        screens_owned[i] = NULL;
    }
    free(work_owned);
    work_owned = NULL;
    sink = NULL;
}

static scene_mesh_t*
mesh_new(void) {
    suite_set_test_cleanup(cleanup);
    for (size_t i = 0; i < MESHES_MAX; i++) {
        if (meshes_owned[i] == NULL) {
            meshes_owned[i] = calloc(1, sizeof(scene_mesh_t));
            TEST_ASSERT_NOT_NULL(meshes_owned[i]);
            for (int r = 0; r < ATLAS_ROWS; r++) {
                meshes_owned[i]->atlas[r] = (uint32_t)(r + 1) * ATLAS_ROW_HASH;
            }
            return meshes_owned[i];
        }
    }
    TEST_FAIL_MESSAGE("a test asked for more meshes than it was given room for");
    return NULL;
}

static screen_t*
screen_new(void) {
    suite_set_test_cleanup(cleanup);
    for (size_t i = 0; i < sizeof screens_owned / sizeof screens_owned[0]; i++) {
        if (screens_owned[i] == NULL) {
            screens_owned[i] = calloc(1, sizeof(screen_t));
            TEST_ASSERT_NOT_NULL(screens_owned[i]);
            return screens_owned[i];
        }
    }
    TEST_FAIL_MESSAGE("a test asked for more screens than it was given room for");
    return NULL;
}

/* Frees the meshes and screens a check made, for the next. */
static void
reset_scene(void) {
    for (size_t i = 0; i < MESHES_MAX; i++) {
        free(meshes_owned[i]);
        meshes_owned[i] = NULL;
    }
    for (size_t i = 0; i < sizeof screens_owned / sizeof screens_owned[0]; i++) {
        free(screens_owned[i]);
        screens_owned[i] = NULL;
    }
}

/* The mesh's clusters share one node; `lit` is how the mesh is coloured. */
static void
finish_mesh(scene_mesh_t* m, bool face_coloured, int cluster_count, int vertex_count, int triangle_count,
            int mask_vertex_first) {
    box3s_t bounds = box3s_empty();
    for (int i = 0; i < vertex_count; i++) {
        bounds = box3s_extend(bounds, vec3s_from_array(m->positions[i]));
    }
    m->nodes[0] = (r3d_lit_node_t){bounds, 0, (uint8_t)cluster_count, true};
    m->mesh = (r3d_lit_mesh_t){.positions = m->positions,
                               .colors = face_coloured ? NULL : m->colors,
                               .triangles = m->triangles,
                               .clusters = m->clusters,
                               .nodes = m->nodes,
                               .vertex_count = vertex_count,
                               .triangle_count = triangle_count,
                               .cluster_count = cluster_count,
                               .node_count = 1,
                               .position_scale = 1,
                               .face_colors = face_coloured ? m->face_colors : NULL,
                               .mask_uvs = m->uvs,
                               .mask_vertex_first = mask_vertex_first,
                               .mask = {m->atlas, ATLAS_ROWS}};
}

static box3s_t
quad_bounds(const int16_t (*corners)[3]) {
    box3s_t bounds = box3s_empty();
    for (int i = 0; i < QUAD_CORNERS; i++) {
        bounds = box3s_extend(bounds, vec3s_from_array(corners[i]));
    }
    return bounds;
}

/* A quad's corners, a triangle pair over them, facing the camera or away. */
static void
set_quad(scene_mesh_t* m, int cluster, const int16_t (*corners)[3], bool facing_away, bool double_sided, bool masked,
         uint16_t face_color, const uint8_t rgb[3]) {
    const int first = cluster * QUAD_CORNERS;
    for (int i = 0; i < QUAD_CORNERS; i++) {
        memcpy(m->positions[first + i], corners[i], sizeof m->positions[0]);
        memcpy(m->colors[first + i], rgb, 3);
    }
    static const uint16_t front[QUAD_TRIANGLES][3] = {{0, 1, 2}, {0, 2, 3}};
    static const uint16_t back[QUAD_TRIANGLES][3] = {{0, 2, 1}, {0, 3, 2}};
    for (int t = 0; t < QUAD_TRIANGLES; t++) {
        for (int k = 0; k < 3; k++) {
            m->triangles[(cluster * QUAD_TRIANGLES) + t][k] = (uint16_t)(first + (facing_away ? back : front)[t][k]);
        }
        m->face_colors[(cluster * QUAD_TRIANGLES) + t] = face_color;
    }
    m->clusters[cluster] = (r3d_lit_cluster_t){
        (uint16_t)first, QUAD_CORNERS, (uint16_t)(cluster * QUAD_TRIANGLES), QUAD_TRIANGLES, quad_bounds(corners),
        double_sided,    masked};
}

/* A quad over pixels [x0, x1) by [y0, y1) at the camera's distance, corners in the order the mask's
 * planes below assume: left-bottom, right-bottom, right-top, left-top, as the world's y runs up. */
static void
screen_corners(int x0, int x1, int y0, int y1, int16_t corners[QUAD_CORNERS][3]) {
    const int16_t left = (int16_t)TICKS_OF(x0 - CENTRE_X);
    const int16_t right = (int16_t)TICKS_OF(x1 - CENTRE_X);
    const int16_t bottom = (int16_t)TICKS_OF(CENTRE_Y - y1);
    const int16_t top = (int16_t)TICKS_OF(CENTRE_Y - y0);
    const int16_t at[QUAD_CORNERS][3] = {{left, bottom, 0}, {right, bottom, 0}, {right, top, 0}, {left, top, 0}};
    memcpy(corners, at, sizeof at);
}

/* The masked quad's u, v per corner, in texels: both differ along each side, and not by whole texels a pixel. */
static void
set_masked_uvs(scene_mesh_t* m, int first_slot, const float u[QUAD_CORNERS], const float v[QUAD_CORNERS]) {
    for (int i = 0; i < QUAD_CORNERS; i++) {
        m->uvs[first_slot + i][0] = (uint16_t)lroundf(u[i] * UV_UNIT);
        m->uvs[first_slot + i][1] = (uint16_t)lroundf(v[i] * UV_UNIT);
    }
}

/* The G1 mesh: the opaque quad, then the masked one, whose vertices start at MASK_VERTEX_FIRST. */
static scene_mesh_t*
g1_mesh(bool face_coloured, bool double_sided) {
    scene_mesh_t* m = mesh_new();
    int16_t corners[QUAD_CORNERS][3];
    static const uint8_t green[3] = {0, UINT8_MAX, 0};
    static const uint8_t red[3] = {UINT8_MAX, 0, 0};
    screen_corners(OPAQUE_X0, OPAQUE_X1, QUAD_Y0, QUAD_Y1, corners);
    set_quad(m, 0, corners, false, double_sided, false, OPAQUE_COLOR, green);
    screen_corners(MASKED_X0, MASKED_X1, QUAD_Y0, QUAD_Y1, corners);
    set_quad(m, 1, corners, false, double_sided, true, MASKED_COLOR, red);
    /* Along x u rises 18 texels and v 3; along the screen's y, u 6 and v 21: 0.75, 0.125, 0.25 and 0.875
     * a pixel, so no pixel's centre falls on a texel edge. */
    static const float u[QUAD_CORNERS] = {1.0625F + 6.0F, 1.0625F + 6.0F + 18.0F, 1.0625F + 18.0F, 1.0625F};
    static const float v[QUAD_CORNERS] = {3.0625F + 21.0F, 3.0625F + 21.0F + 3.0F, 3.0625F + 3.0F, 3.0625F};
    set_masked_uvs(m, 0, u, v);
    finish_mesh(m, face_coloured, CLUSTERS, MESH_VERTICES, MESH_TRIANGLES, MASK_VERTEX_FIRST);
    return m;
}

static void
record_written(const r3d_span_writer_t* writer, const gfx_target_t* rows, int y, int first, int last, int32_t z,
               int32_t dz) {
    for (int x = first; x <= last; x++) {
        const int at = (y * SCREEN_W) + x;
        sink->raw[at]++;
        /* As a writer does: the pixels the draw won are those whose depth is the span's. */
        sink->hits[at] += (uint16_t)((z + (dz * (x - first))) >> R3D_DEPTH_SHIFT) == sink->depth[at];
    }
}

/* Depth before a draw, cycling nearer than the quads (they lose), their own (a tie loses) and empty. */
static uint16_t
depth_before(int i) {
    const int kind = i % DEPTH_KINDS;
    return kind == 0 ? R3D_DEPTH_NEAREST : kind == 1 ? (uint16_t)(R3D_DEPTH_NEAREST / 2) : R3D_DEPTH_EMPTY;
}

static r3d_lens_t
lens_at(float eye_z, float near_z) {
    r3d_lens_t lens;
    const render_view_t view = render_view_fixture_at(&(fixture_camera_t){{0, 0, eye_z}, {0, 0, 1}, 0.5F, near_z},
                                                      (viewport_t){SCREEN_W, SCREEN_H, 0});
    r3d_lens_init(&lens, &view, 1);
    return lens;
}

/* Draws every visible cluster of `m` into `s`; with `writing`, through a writer that counts the pixels it is
 * handed, over depth the quads only partly win. */
static void
draw_mesh(const scene_mesh_t* m, const r3d_lens_t* lens, screen_t* s, bool writing, bool partly_hidden) {
    if (work_owned == NULL) {
        work_owned = malloc(r3d_pipeline_work_bytes());
    }
    suite_set_test_cleanup(cleanup);
    TEST_ASSERT_NOT_NULL(work_owned);
    for (int i = 0; i < SCREEN_PIXELS; i++) {
        s->depth[i] = partly_hidden ? depth_before(i) : R3D_DEPTH_EMPTY;
    }
    r3d_span_target_t target = r3d_span_target(s->color, s->depth, SCREEN_W, 0, SCREEN_H);
    const r3d_span_writer_t writer = {.span = record_written};
    if (writing) {
        target.writers = &writer;
        target.writer_count = 1;
        sink = s;
    }
    uint16_t visible[CLUSTERS];
    const int count = r3d_pipeline_cull(&m->mesh, lens, NULL, visible, work_owned, NULL);
    TEST_ASSERT_EQUAL_INT(m->mesh.cluster_count, count);
    r3d_pipeline_transform(&m->mesh, lens, visible, count, s->cs, NULL, false);
    r3d_pipeline_draw(&m->mesh, lens, visible, count, s->cs, NULL, &target, work_owned);
}

/* value = c + x px + y py over the screen, through three points. */
typedef struct {
    float c, x, y;
} plane_t;

static plane_t
plane_through(const float px[3], const float py[3], const float value[3]) {
    const float e1x = px[1] - px[0];
    const float e1y = py[1] - py[0];
    const float e2x = px[2] - px[0];
    const float e2y = py[2] - py[0];
    const float det = (e1x * e2y) - (e2x * e1y);
    const float d1 = value[1] - value[0];
    const float d2 = value[2] - value[0];
    plane_t p = {0, ((d1 * e2y) - (d2 * e1y)) / det, ((d2 * e1x) - (d1 * e2x)) / det};
    p.c = value[0] - (p.x * px[0]) - (p.y * py[0]);
    return p;
}

static int
clamped(float texel, int count) {
    const int at = (int)floorf(texel);
    return at < 0 ? 0 : at >= count ? count - 1 : at;
}

/* The mask's two planes over the screen for quad vertices [first, first + 4) of a mesh transformed into `s`:
 * through the corners the lens put them at. */
typedef struct {
    plane_t u, v;
} mask_planes_t;

static mask_planes_t
masked_planes(const scene_mesh_t* m, const screen_t* s, int first_vertex) {
    const int slot = first_vertex - m->mesh.mask_vertex_first;
    float px[3];
    float py[3];
    float u[3];
    float v[3];
    static const int corner[3] = {0, 1, 3};
    for (int k = 0; k < 3; k++) {
        const r3d_pipeline_vertex_t* at = &s->cs[first_vertex + corner[k]];
        px[k] = (float)at->sx / (float)R3D_SUBPIXEL;
        py[k] = (float)at->sy / (float)R3D_SUBPIXEL;
        u[k] = (float)m->uvs[slot + corner[k]][0] / (float)UV_UNIT;
        v[k] = (float)m->uvs[slot + corner[k]][1] / (float)UV_UNIT;
    }
    return (mask_planes_t){plane_through(px, py, u), plane_through(px, py, v)};
}

static bool
kept_at(const scene_mesh_t* m, const mask_planes_t* planes, int x, int y) {
    const float px = (float)x + PIXEL_CENTRE;
    const float py = (float)y + PIXEL_CENTRE;
    const float u = planes->u.c + (planes->u.x * px) + (planes->u.y * py);
    const float v = planes->v.c + (planes->v.x * px) + (planes->v.y * py);
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(EDGE_MARGIN, fabsf(u - roundf(u)), "a sample sits on a texel edge in u");
    TEST_ASSERT_GREATER_THAN_FLOAT_MESSAGE(EDGE_MARGIN, fabsf(v - roundf(v)), "a sample sits on a texel edge in v");
    return ((m->atlas[clamped(v, ATLAS_ROWS)] >> clamped(u, (int)LMSK_MASK_WIDTH)) & 1U) != 0;
}

static bool
in_masked_quad(int x, int y) {
    return x >= MASKED_X0 && x < MASKED_X1 && y >= QUAD_Y0 && y < QUAD_Y1;
}

static bool
in_opaque_quad(int x, int y) {
    return x >= OPAQUE_X0 && x < OPAQUE_X1 && y >= QUAD_Y0 && y < QUAD_Y1;
}

/* The corners landed where the quads were meant to span: the planes' oracle rests on it. */
static void
assert_quads_where_built(const screen_t* s) {
    TEST_ASSERT_EQUAL_INT(MASKED_X0 * R3D_SUBPIXEL, s->cs[MASK_VERTEX_FIRST].sx);
    TEST_ASSERT_EQUAL_INT(QUAD_Y1 * R3D_SUBPIXEL, s->cs[MASK_VERTEX_FIRST].sy);
    TEST_ASSERT_EQUAL_INT(MASKED_X1 * R3D_SUBPIXEL, s->cs[MASK_VERTEX_FIRST + 1].sx);
    TEST_ASSERT_EQUAL_INT(QUAD_Y0 * R3D_SUBPIXEL, s->cs[MASK_VERTEX_FIRST + 3].sy);
}

/* What a draw of the G1 mesh must leave: the opaque quad whole, the masked one only where the atlas keeps it
 * at the u, v the vertices make, and, over partly hidden depth, only where depth lets each win. */
static void
assert_g1_pixels(const scene_mesh_t* m, const screen_t* s, bool partly_hidden, bool writing) {
    const mask_planes_t planes = masked_planes(m, s, MASK_VERTEX_FIRST);
    int kept = 0;
    int dropped = 0;
    for (int y = 0; y < SCREEN_H; y++) {
        for (int x = 0; x < SCREEN_W; x++) {
            const int i = (y * SCREEN_W) + x;
            const bool wins = !partly_hidden || depth_before(i) == R3D_DEPTH_EMPTY;
            uint16_t color = 0;
            if (in_opaque_quad(x, y)) {
                color = wins ? OPAQUE_COLOR : 0;
            } else if (in_masked_quad(x, y)) {
                const bool keeps = kept_at(m, &planes, x, y);
                color = keeps && wins ? MASKED_COLOR : 0;
                kept += keeps;
                dropped += !keeps;
            }
            TEST_ASSERT_EQUAL_HEX16(color, s->color[i]);
            const uint16_t before = partly_hidden ? depth_before(i) : R3D_DEPTH_EMPTY;
            TEST_ASSERT_EQUAL_INT(color != 0, s->depth[i] != before);
            TEST_ASSERT_EQUAL_UINT16(writing && color != 0 ? 1 : 0, s->hits[i]);
            if (in_masked_quad(x, y)) { /* the masked fill hands its writers its winners only */
                TEST_ASSERT_EQUAL_UINT16(writing && color != 0 ? 1 : 0, s->raw[i]);
            }
        }
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, kept, "the mask kept nothing");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, dropped, "the mask dropped nothing");
}

static void
draw_g1(bool face_coloured, bool writing, bool partly_hidden) {
    reset_scene();
    const scene_mesh_t* m = g1_mesh(face_coloured, false);
    const r3d_lens_t lens = lens_at(CAMERA_BACK, 1.0F);
    screen_t* s = screen_new();
    draw_mesh(m, &lens, s, writing, partly_hidden);
    assert_quads_where_built(s);
    assert_g1_pixels(m, s, partly_hidden, writing);
}

/* Only the masked cluster's dropped texels leave the target untouched, the opaque cluster beside it drawn
 * whole: the cluster's vertices come after the opaque ones, so each u, v is found past mask_vertex_first. */
static void
test_a_masked_cluster_of_a_face_coloured_mesh_draws_only_the_texels_its_u_v_keep(void) {
    draw_g1(true, false, false);
}

static void
test_a_masked_cluster_of_a_vertex_coloured_mesh_draws_only_the_texels_its_u_v_keep(void) {
    draw_g1(false, false, false);
}

/* A writer is handed the kept pixels that won the depth test and no other. */
static void
test_writers_see_only_the_kept_depth_winning_pixels_of_a_masked_cluster(void) {
    draw_g1(true, true, true);
    draw_g1(false, true, true);
}

/* The writing draw and the reading draw, over the same depth, leave the same colour and depth. */
static void
test_a_writing_draw_of_a_masked_cluster_leaves_the_pixels_of_the_reading_draw(void) {
    for (int face = 0; face < 2; face++) {
        reset_scene();
        const scene_mesh_t* m = g1_mesh(face != 0, false);
        const r3d_lens_t lens = lens_at(CAMERA_BACK, 1.0F);
        screen_t* reading = screen_new();
        screen_t* writing = screen_new();
        draw_mesh(m, &lens, reading, false, true);
        draw_mesh(m, &lens, writing, true, true);
        TEST_ASSERT_EQUAL_MEMORY(reading->color, writing->color, sizeof reading->color);
        TEST_ASSERT_EQUAL_MEMORY(reading->depth, writing->depth, sizeof reading->depth);
    }
}

/* G2: a quad over the view that the near plane cuts. Its corners, in ticks 1000 from the eye, tilted a few
 * ticks in depth so that the plane cuts through the middle, and wide and tall enough that the cut part holds
 * many pixels. */
#define CLIP_DISTANCE    1000
#define CLIP_TILT        5
#define CLIP_HALF_WIDTH  625
#define CLIP_HALF_HEIGHT 500

static scene_mesh_t*
g2_mesh(bool facing_away, bool double_sided, bool masked, bool face_coloured) {
    scene_mesh_t* m = mesh_new();
    const int16_t corners[QUAD_CORNERS][3] = {{-CLIP_HALF_WIDTH, -CLIP_HALF_HEIGHT, CLIP_DISTANCE - CLIP_TILT},
                                              {CLIP_HALF_WIDTH, -CLIP_HALF_HEIGHT, CLIP_DISTANCE - CLIP_TILT},
                                              {CLIP_HALF_WIDTH, CLIP_HALF_HEIGHT, CLIP_DISTANCE + CLIP_TILT},
                                              {-CLIP_HALF_WIDTH, CLIP_HALF_HEIGHT, CLIP_DISTANCE + CLIP_TILT}};
    static const uint8_t white[3] = {UINT8_MAX, UINT8_MAX, UINT8_MAX};
    set_quad(m, 0, corners, facing_away, double_sided, masked, MASKED_COLOR, white);
    /* Over the 60 by 48 pixels, u rises 15 texels along x and 12 up the screen, v 7.5 along x and 12 down it:
     * a quarter, an eighth, a quarter and an eighth of a texel a pixel, so no sample is near a texel edge. */
    static const float u[QUAD_CORNERS] = {0.375F, 15.375F, 27.375F, 12.375F};
    static const float v[QUAD_CORNERS] = {14.0625F, 21.5625F, 9.5625F, 2.0625F};
    set_masked_uvs(m, 0, u, v);
    finish_mesh(m, face_coloured, 1, QUAD_CORNERS, QUAD_TRIANGLES, 0);
    return m;
}

/* The pixels of `s` the quad covers, by depth, and, of those, the ones the mask keeps by colour. */
static int
count_depth(const screen_t* s) {
    int n = 0;
    for (int i = 0; i < SCREEN_PIXELS; i++) {
        n += s->depth[i] != R3D_DEPTH_EMPTY;
    }
    return n;
}

static void
check_clipped_masked_quad(bool facing_away, bool double_sided, bool face_coloured) {
    reset_scene();
    /* Three draws of the same quad: unmasked cut by the near plane, for where it is seen; masked and cut; masked
     * and whole, for the planes the cut one must still follow. */
    const scene_mesh_t* plain = g2_mesh(facing_away, double_sided, false, face_coloured);
    const r3d_lens_t cutting = lens_at(0, (float)CLIP_DISTANCE);
    const r3d_lens_t whole = lens_at(0, 1.0F);
    screen_t* region = screen_new();
    draw_mesh(plain, &cutting, region, false, false);
    const int seen = count_depth(region);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(SCREEN_PIXELS / 8, seen, "the cut quad covers too little to tell");
    TEST_ASSERT_LESS_THAN_INT_MESSAGE(SCREEN_PIXELS, seen, "the cut quad covers everything");

    const scene_mesh_t* masked = g2_mesh(facing_away, double_sided, true, face_coloured);
    screen_t* clipped = screen_new();
    screen_t* reference = screen_new();
    draw_mesh(masked, &cutting, clipped, false, false);
    draw_mesh(masked, &whole, reference, false, false);

    const mask_planes_t planes = masked_planes(masked, reference, 0);
    int kept = 0;
    int dropped = 0;
    for (int i = 0; i < SCREEN_PIXELS; i++) {
        if (region->depth[i] != R3D_DEPTH_EMPTY) {
            (void)kept_at(masked, &planes, i % SCREEN_W, i / SCREEN_W); /* no sample on a texel edge */
        }
        if (region->depth[i] == R3D_DEPTH_EMPTY) {
            TEST_ASSERT_EQUAL_HEX16(0, clipped->color[i]);
            TEST_ASSERT_EQUAL_UINT16(R3D_DEPTH_EMPTY, clipped->depth[i]);
            continue;
        }
        TEST_ASSERT_EQUAL_HEX16(reference->color[i], clipped->color[i]);
        kept += clipped->color[i] != 0;
        dropped += clipped->color[i] == 0;
    }
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(seen / 4, kept, "the mask kept too little of the cut quad");
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(seen / 4, dropped, "the mask dropped too little of the cut quad");
}

/* A masked quad the near plane cuts, clipped into a fan, keeps the pixels the same quad keeps uncut: each
 * fan triangle's corners take the u, v of where they lie on the source triangle. */
static void
test_a_masked_quad_cut_by_the_near_plane_keeps_the_texels_it_keeps_uncut(void) {
    check_clipped_masked_quad(false, false, false);
    check_clipped_masked_quad(false, false, true);
}

static void
test_a_double_sided_masked_quad_cut_by_the_near_plane_keeps_the_texels_it_keeps_uncut(void) {
    check_clipped_masked_quad(false, true, false);
    check_clipped_masked_quad(false, true, true);
    check_clipped_masked_quad(true, true, false);
}

void
run_masked_pipeline_suite(void) {
    RUN_TEST(test_a_masked_cluster_of_a_face_coloured_mesh_draws_only_the_texels_its_u_v_keep);
    RUN_TEST(test_a_masked_cluster_of_a_vertex_coloured_mesh_draws_only_the_texels_its_u_v_keep);
    RUN_TEST(test_writers_see_only_the_kept_depth_winning_pixels_of_a_masked_cluster);
    RUN_TEST(test_a_writing_draw_of_a_masked_cluster_leaves_the_pixels_of_the_reading_draw);
    RUN_TEST(test_a_masked_quad_cut_by_the_near_plane_keeps_the_texels_it_keeps_uncut);
    RUN_TEST(test_a_double_sided_masked_quad_cut_by_the_near_plane_keeps_the_texels_it_keeps_uncut);
}

SUITE_REGISTER(run_masked_pipeline_suite);
