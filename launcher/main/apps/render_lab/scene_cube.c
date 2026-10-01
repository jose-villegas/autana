/*
 * scene_cube - the Gouraud-shaded rotating RGB cube, as a render_lab scene.
 *
 * small3dlib projects the cube and culls its back faces; render/'s span
 * rasterizer fills it. Its depth plane is one band tall (GFX_BAND_HEIGHT
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
#include "render/r3d_span.h"
#include "render_lab.h"
#include "render_lab_scene.h"

/* small3dlib config: must precede its include. It rasterizes nothing here. */
#define S3L_PIXEL_FUNCTION     cube_unused_pixel
#define S3L_RESOLUTION_X       GFX_WIDTH
#define S3L_RESOLUTION_Y       GFX_HEIGHT
#define S3L_Z_BUFFER           0
#define S3L_SORT               0
#define S3L_MAX_TRIANGES_DRAWN 1 /* never drawn; small3dlib still sizes an array off this */
#include "small3dlib.h"

static inline void
cube_unused_pixel(S3L_PixelInfo* pixel) {
    (void)pixel;
}

/* small3dlib is fixed point: S3L_F (512) is 1.0, and is also one full turn
 * when used as an angle. */

#define CUBE_DISTANCE       (3 * S3L_F)
#define CAMERA_FOCAL_LENGTH (2 * S3L_F)

/* Milliseconds per revolution. Deliberately unequal so it tumbles rather than
 * spinning about one fixed axis. */
#define SPIN_PERIOD_Y_MS    4000
#define SPIN_PERIOD_X_MS    7000

static const S3L_Unit cube_vertices[] = {S3L_CUBE_VERTICES(S3L_F)};
static const S3L_Index cube_triangles[] = {S3L_CUBE_TRIANGLES};

/* Each corner is coloured by the sign of its position: +x adds red, +y green,
 * +z blue. Interpolating those across each face gives the gradients. */
static const uint8_t cube_corner_colors[S3L_CUBE_VERTEX_COUNT][3] = {
    {255, 0, 0},     /* 0  right, bottom, front */
    {0, 0, 0},       /* 1  left,  bottom, front */
    {255, 255, 0},   /* 2  right, top,    front */
    {0, 255, 0},     /* 3  left,  top,    front */
    {255, 0, 255},   /* 4  right, bottom, back  */
    {0, 0, 255},     /* 5  left,  bottom, back  */
    {255, 255, 255}, /* 6  right, top,    back  */
    {0, 255, 255},   /* 7  left,  top,    back  */
};

static S3L_Model3D cube;
static S3L_Scene scene;
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
#define CUBE_NEAR_DEPTH ((float)S3L_NEAR)

void
cube_update_rotation(uint32_t dt_ms) {
    elapsed_ms += dt_ms;

    cube.transform.rotation.y = (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_Y_MS) % S3L_F);
    cube.transform.rotation.x = (S3L_Unit)(((uint64_t)elapsed_ms * S3L_F / SPIN_PERIOD_X_MS) % S3L_F);
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

static cube_triangle_bin_t cube_bin[S3L_CUBE_TRIANGLE_COUNT];
static int cube_bin_count;

/* small3dlib samples a pixel at its integer coordinate; the span rasterizer
 * at the pixel's centre, half a pixel further. */
static r3d_span_vertex_t
cube_span_vertex(S3L_Vec4 projected, S3L_Index corner) {
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
cube_triangle_bounds(const S3L_Vec4 transformed[6], int* x0, int* x1, int* y0, int* y1) {
    *x0 = transformed[0].x;
    *x1 = transformed[0].x;
    *y0 = transformed[0].y;
    *y1 = transformed[0].y;
    for (int i = 1; i < 3; i++) {
        const S3L_Unit x = transformed[i].x;
        const S3L_Unit y = transformed[i].y;
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

/* Projects every visible triangle once per frame. Only correct while
 * S3L_NEAR_CROSS_STRATEGY stays 0, so no triangle is split at the near
 * plane (asserted). The depth test resolves overlap, so the bin needs no
 * order; y1 is exclusive, hence the +1 on the inclusive row. */
static void
cube_bin_triangles(void) {
    S3L_Mat4 mat_camera, mat_final;

    assert(cube.customTransformMatrix == 0);

    S3L_makeCameraMatrix(scene.camera.transform, mat_camera);
    S3L_makeWorldMatrix(cube.transform, mat_final);
    S3L_mat4Xmat4(mat_final, mat_camera);

    cube_bin_count = 0;
    cube_bbox_valid = false;

    for (S3L_Index t = 0; t < S3L_CUBE_TRIANGLE_COUNT; t++) {
        S3L_Vec4 transformed[6];

        _S3L_projectTriangle(&cube, t, mat_final, scene.camera.focalLength, transformed);
        assert(_S3L_projectedTriangleState == 0);

        if (!S3L_triangleIsVisible(transformed[0], transformed[1], transformed[2], cube.config.backfaceCulling)) {
            continue;
        }

        int x0, x1, y0, y1;
        cube_triangle_bounds(transformed, &x0, &x1, &y0, &y1);
        x0 = x0 < 0 ? 0 : x0;
        x1 = (x1 + 1 > GFX_WIDTH) ? GFX_WIDTH : x1 + 1;

        cube_triangle_bin_t* entry = &cube_bin[cube_bin_count++];
        for (int i = 0; i < 3; i++) {
            entry->v[i] = cube_span_vertex(transformed[i], cube_triangles[(t * 3) + i]);
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

/* S3L_sceneInit() resets the camera, so the focal length override comes after
 * it. Enlarge by zooming rather than moving the cube closer: the near plane
 * would clip the front faces long before it filled the screen, and a clipped
 * triangle is discarded. A box from a previous visit is not "last frame". */
static void
scene_cube_enter(void) {
    S3L_model3DInit(cube_vertices, S3L_CUBE_VERTEX_COUNT, cube_triangles, S3L_CUBE_TRIANGLE_COUNT, &cube);
    cube.transform.translation.z = CUBE_DISTANCE;

    S3L_sceneInit(&cube, 1, &scene);

    scene.camera.focalLength = CAMERA_FOCAL_LENGTH;

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
