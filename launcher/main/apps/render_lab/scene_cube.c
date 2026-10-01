/*
 * scene_cube - the Gouraud-shaded rotating RGB cube, as a render_lab scene.
 *
 * The cube is projected and its back faces culled in mat4i's fixed point;
 * render/'s span rasterizer fills it. Its depth plane is one band tall (GFX_BAND_HEIGHT
 * rows), reused by every band and strip: a full colour+depth pair is ~1.3 MB
 * on a chip with ~424 KiB of RAM.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "render/r3d_line_camera.h"
#include "render/r3d_span.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "util/mat4i.h"

/* One unit is M4_ONE, and M4_ONE is also one full turn as an angle. */

#define CUBE_DISTANCE       (3 * M4_ONE)
#define CAMERA_FOCAL_LENGTH (2 * M4_ONE)

/* A triangle with a corner this close or closer is culled, not clipped. */
#define CUBE_NEAR_Z         (M4_ONE / 4)

/* Milliseconds per revolution. Deliberately unequal so it tumbles rather than
 * spinning about one fixed axis. */
#define SPIN_PERIOD_Y_MS    4000
#define SPIN_PERIOD_X_MS    7000

#define CUBE_VERTEX_COUNT   8
#define CUBE_TRIANGLE_COUNT 12
#define CUBE_HALF           (M4_ONE / 2)

static const m4_unit_t cube_vertices[CUBE_VERTEX_COUNT][3] = {
    {CUBE_HALF, -CUBE_HALF, -CUBE_HALF}, {-CUBE_HALF, -CUBE_HALF, -CUBE_HALF}, {CUBE_HALF, CUBE_HALF, -CUBE_HALF},
    {-CUBE_HALF, CUBE_HALF, -CUBE_HALF}, {CUBE_HALF, -CUBE_HALF, CUBE_HALF},   {-CUBE_HALF, -CUBE_HALF, CUBE_HALF},
    {CUBE_HALF, CUBE_HALF, CUBE_HALF},   {-CUBE_HALF, CUBE_HALF, CUBE_HALF},
};

/* Front, right, back, left, top, bottom: two triangles each. */
static const uint8_t cube_triangles[CUBE_TRIANGLE_COUNT][3] = {
    {3, 0, 2}, {1, 0, 3}, {0, 4, 2}, {2, 4, 6}, {4, 5, 6}, {7, 6, 5},
    {3, 7, 1}, {1, 7, 5}, {6, 3, 2}, {7, 3, 6}, {1, 4, 0}, {5, 4, 1},
};

/* Each corner is coloured by the sign of its position: +x adds red, +y green,
 * +z blue. Interpolating those across each face gives the gradients. */
static const uint8_t cube_corner_colors[CUBE_VERTEX_COUNT][3] = {
    {255, 0, 0},     /* 0  right, bottom, front */
    {0, 0, 0},       /* 1  left,  bottom, front */
    {255, 255, 0},   /* 2  right, top,    front */
    {0, 255, 0},     /* 3  left,  top,    front */
    {255, 0, 255},   /* 4  right, bottom, back  */
    {0, 0, 255},     /* 5  left,  bottom, back  */
    {255, 255, 255}, /* 6  right, top,    back  */
    {0, 255, 255},   /* 7  left,  top,    back  */
};

static m4_transform_t cube_pose;
static m4_transform_t camera_pose;
static uint32_t elapsed_ms;

/* This frame's cube coverage and last frame's: band mode marks both dirty,
 * because a band the cube left still needs erasing. */
static int cube_bbox_x0, cube_bbox_y0, cube_bbox_x1, cube_bbox_y1;
static bool cube_bbox_valid;
static render_lab_coverage_t last_coverage;

/* The span rasterizer's depth plane for one band, GFX_BAND_HEIGHT x
 * GFX_WIDTH x 2 bytes in internal RAM; NULL when it did not allocate, and
 * the cube then draws nothing. */
static uint16_t* band_depth;
static const char* TAG = "scene_cube";
_Static_assert(GFX_HEIGHT % GFX_BAND_HEIGHT == 0, "the full-frame strips are whole bands");
_Static_assert(R3D_DEPTH_EMPTY == 0, "a band's depth is cleared with memset");

/* Inverse depth is the near plane over the camera-space depth, so (0, 1] for
 * everything the near plane keeps. */
#define CUBE_NEAR_DEPTH ((float)CUBE_NEAR_Z)

void
cube_update_rotation(uint32_t dt_ms) {
    elapsed_ms += dt_ms;

    cube_pose.rotation.y = (m4_unit_t)(((uint64_t)elapsed_ms * M4_ONE / SPIN_PERIOD_Y_MS) % M4_ONE);
    cube_pose.rotation.x = (m4_unit_t)(((uint64_t)elapsed_ms * M4_ONE / SPIN_PERIOD_X_MS) % M4_ONE);
}

void
cube_clear_frame(void) {
    /* gfx_set_partial_clear() delegates bounding-box erase and dirty marking
     * of previous-frame bounds directly to gfx_clear(). */
    gfx_set_partial_clear(render_lab_partial_updates);
    gfx_clear(gfx_rgb(RENDER_LAB_BACKGROUND_RGB));
}

/* One visible triangle, projected once per frame; see cube_bin_triangles().
 * y0/y1 is its screen-space row extent, so a band can test overlap without
 * rasterizing. */
typedef struct {
    r3d_span_vertex_t v[3];
    int y0, y1;
} cube_triangle_bin_t;

static cube_triangle_bin_t cube_bin[CUBE_TRIANGLE_COUNT];
static int cube_bin_count;

/* A projected vertex is a whole pixel coordinate; the span rasterizer samples
 * at the pixel's centre, half a pixel further. */
static r3d_span_vertex_t
cube_span_vertex(m4_vec4_t projected, int corner) {
    const uint8_t* rgb = cube_corner_colors[corner];
    r3d_span_vertex_t v;
    v.x = (projected.x * R3D_SUBPIXEL) + (R3D_SUBPIXEL / 2);
    v.y = (projected.y * R3D_SUBPIXEL) + (R3D_SUBPIXEL / 2);
    v.z = CUBE_NEAR_DEPTH / (float)projected.w;
    v.r = (float)rgb[0];
    v.g = (float)rgb[1];
    v.b = (float)rgb[2];
    return v;
}

static void
cube_triangle_bounds(const m4_vec4_t transformed[3], int* x0, int* x1, int* y0, int* y1) {
    *x0 = transformed[0].x;
    *x1 = transformed[0].x;
    *y0 = transformed[0].y;
    *y1 = transformed[0].y;
    for (int i = 1; i < 3; i++) {
        const m4_unit_t x = transformed[i].x;
        const m4_unit_t y = transformed[i].y;
        if (x < *x0) {
            *x0 = x;
        }
        if (x > *x1) {
            *x1 = x;
        }
        if (y < *y0) {
            *y0 = y;
        }
        if (y > *y1) {
            *y1 = y;
        }
    }
}

static void
cube_expand_bbox(const cube_triangle_bin_t* entry, int x0, int x1) {
    if (!cube_bbox_valid) {
        cube_bbox_x0 = x0;
        cube_bbox_y0 = entry->y0;
        cube_bbox_x1 = x1;
        cube_bbox_y1 = entry->y1;
        cube_bbox_valid = true;
    } else {
        if (x0 < cube_bbox_x0) {
            cube_bbox_x0 = x0;
        }
        if (entry->y0 < cube_bbox_y0) {
            cube_bbox_y0 = entry->y0;
        }
        if (x1 > cube_bbox_x1) {
            cube_bbox_x1 = x1;
        }
        if (entry->y1 > cube_bbox_y1) {
            cube_bbox_y1 = entry->y1;
        }
    }
}

/* The vertex in pixels; z and w both keep the camera-space depth. */
static m4_vec4_t
cube_project(int corner, const r3d_line_view_t* view) {
    const m4_vec4_t model = {cube_vertices[corner][0], cube_vertices[corner][1], cube_vertices[corner][2], M4_ONE};
    const m4_vec4_t camera = r3d_to_camera_space(model, view);
    int x, y;
    r3d_camera_to_screen(camera, view, &x, &y);
    return (m4_vec4_t){x, y, camera.z, camera.z};
}

/* False for a triangle that touches the near plane, lies wholly off one side
 * of the screen, or faces away (clockwise on screen). */
static bool
cube_triangle_visible(const m4_vec4_t p[3]) {
    bool off_left = true;
    bool off_right = true;
    bool off_top = true;
    bool off_bottom = true;
    for (int i = 0; i < 3; i++) {
        if (p[i].z <= CUBE_NEAR_Z) {
            return false;
        }
        off_left = off_left && p[i].x < 0;
        off_right = off_right && p[i].x >= GFX_WIDTH;
        off_top = off_top && p[i].y < 0;
        off_bottom = off_bottom && p[i].y > GFX_HEIGHT;
    }
    if (off_left || off_right || off_top || off_bottom) {
        return false;
    }
    const int32_t winding = ((p[1].y - p[0].y) * (p[2].x - p[1].x)) - ((p[1].x - p[0].x) * (p[2].y - p[1].y));
    return winding >= 0;
}

/* Projects every visible triangle once per frame. A triangle crossing the
 * near plane is culled whole. The depth test resolves overlap, so the bin
 * needs no order; y1 is exclusive, hence the +1 on the inclusive row. */
static void
cube_bin_triangles(void) {
    const r3d_line_camera_t camera = {camera_pose, CAMERA_FOCAL_LENGTH, CUBE_NEAR_Z};
    const r3d_line_view_t view = r3d_line_camera_view(camera, cube_pose, (viewport_t){GFX_WIDTH, GFX_HEIGHT, 0});

    cube_bin_count = 0;
    cube_bbox_valid = false;

    for (int t = 0; t < CUBE_TRIANGLE_COUNT; t++) {
        m4_vec4_t transformed[3];
        for (int i = 0; i < 3; i++) {
            transformed[i] = cube_project(cube_triangles[t][i], &view);
        }

        if (!cube_triangle_visible(transformed)) {
            continue;
        }

        int x0, x1, y0, y1;
        cube_triangle_bounds(transformed, &x0, &x1, &y0, &y1);
        x0 = x0 < 0 ? 0 : x0;
        x1 = (x1 + 1 > GFX_WIDTH) ? GFX_WIDTH : x1 + 1;

        cube_triangle_bin_t* entry = &cube_bin[cube_bin_count++];
        for (int i = 0; i < 3; i++) {
            entry->v[i] = cube_span_vertex(transformed[i], cube_triangles[t][i]);
            assert(entry->v[i].x > -R3D_SPAN_RANGE && entry->v[i].x < R3D_SPAN_RANGE);
            assert(entry->v[i].y > -R3D_SPAN_RANGE && entry->v[i].y < R3D_SPAN_RANGE);
        }
        entry->y0 = y0 < 0 ? 0 : y0;
        entry->y1 = (y1 + 1 > GFX_HEIGHT) ? GFX_HEIGHT : y1 + 1;

        cube_expand_bbox(entry, x0, x1);
    }
}

/* Draws the bin's triangles that overlap [row0, row1) into `target`, the
 * first pixel of row0, so a triangle costs only the rows of the band it is
 * in. A band with no triangle skips even the depth clear. */
static void
cube_draw_rows(gfx_color_t* target, int row0, int row1) {
    if (band_depth == NULL) {
        return;
    }
    assert(row1 - row0 <= GFX_BAND_HEIGHT);
    const r3d_span_target_t window = {target, band_depth, GFX_WIDTH, row0, row1};
    bool depth_cleared = false;
    for (int i = 0; i < cube_bin_count; i++) {
        const cube_triangle_bin_t* entry = &cube_bin[i];
        if (entry->y1 <= row0 || entry->y0 >= row1) {
            continue;
        }
        if (!depth_cleared) {
            memset(band_depth, 0, sizeof(*band_depth) * (size_t)(row1 - row0) * GFX_WIDTH);
            depth_cleared = true;
        }
        r3d_span_triangle(&window, &entry->v[0], &entry->v[1], &entry->v[2]);
    }
}

/* Exposed (suite_cube_perf.c) so the perf suite can time the full-frame
 * draw as its own phase. The rasterizer writes straight into
 * gfx_framebuffer(), which gfx cannot see, so the one gfx_mark_dirty() here
 * is what tells it what changed. */
void
cube_rasterize_frame(void) {
    cube_bin_triangles();
    for (int row0 = 0; row0 < GFX_HEIGHT; row0 += GFX_BAND_HEIGHT) {
        cube_draw_rows(gfx_framebuffer() + ((size_t)row0 * GFX_WIDTH), row0, row0 + GFX_BAND_HEIGHT);
    }

    if (render_lab_partial_updates && cube_bbox_valid && cube_bbox_x1 > cube_bbox_x0 && cube_bbox_y1 > cube_bbox_y0) {
        gfx_mark_dirty(cube_bbox_x0, cube_bbox_y0, cube_bbox_x1 - cube_bbox_x0, cube_bbox_y1 - cube_bbox_y0);
    }
}

/* Band mode's per-frame step: project once, and mark the cube's own
 * coverage dirty, before cube_rasterize_band() runs per touched band. Marked
 * here, not by each caller: a band the cube left still needs erasing even
 * though nothing there overlaps this frame's own bbox. */
void
cube_transform_and_bin(void) {
    cube_bin_triangles();

    render_lab_coverage_mark(&last_coverage, cube_bbox_valid, cube_bbox_x0, cube_bbox_y0, cube_bbox_x1, cube_bbox_y1);
}

void
cube_rasterize_band(gfx_color_t* buf, int row0, int row1) {
    cube_draw_rows(buf, row0, row1);
}

/* Enlarge by zooming rather than moving the cube closer: the near plane
 * would clip the front faces long before it filled the screen, and a clipped
 * triangle is discarded. A box from a previous visit is not "last frame". */
static void
scene_cube_enter(void) {
    m4_transform_init(&cube_pose);
    cube_pose.translation.z = CUBE_DISTANCE;
    m4_transform_init(&camera_pose);

    const size_t depth_bytes = sizeof(*band_depth) * (size_t)GFX_BAND_HEIGHT * GFX_WIDTH;
    band_depth = heap_caps_malloc(depth_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (band_depth == NULL) {
        ESP_LOGE(TAG, "no %u bytes of internal RAM for the depth plane: the scene stays blank", (unsigned)depth_bytes);
    }

    elapsed_ms = 0;
    last_coverage.valid = false;
}

static void
scene_cube_frame(uint32_t dt_ms, bool band_mode_active) {
    cube_update_rotation(dt_ms);
    if (band_mode_active) {
        cube_transform_and_bin();
    } else {
        cube_clear_frame();
        cube_rasterize_frame();
    }
}

static void
scene_cube_frame_band(gfx_color_t* buf, int row0, int row1) {
    cube_rasterize_band(buf, row0, row1);
}

static void
scene_cube_exit(void) {
    heap_caps_free(band_depth);
    band_depth = NULL;
}

static void
scene_cube_invalidate(void) {
    last_coverage.valid = false;
}

const render_lab_scene_t scene_cube = {
    .name = "Gouraud Cube",
    .key = "gouraud",
    .enter = scene_cube_enter,
    .frame = scene_cube_frame,
    .frame_band = scene_cube_frame_band,
    .exit = scene_cube_exit,
    .invalidate = scene_cube_invalidate,
};
