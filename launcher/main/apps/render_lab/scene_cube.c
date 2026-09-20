/*
 * scene_cube - the Gouraud-shaded rotating RGB cube, as a render_lab scene.
 *
 * Why small3dlib rather than a conventional rasterizer: it owns no
 * framebuffer (every rasterized pixel comes back through a callback) and with
 * S3L_Z_BUFFER 0 it keeps no depth buffer, resolving visibility by sorting
 * triangles back-to-front. A colour+depth rasterizer would want ~1.3 MB at
 * this resolution, against ~424 KiB of RAM on the whole chip.
 */

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include "../../gfx/gfx.h"
#include "render_lab.h"
#include "render_lab_scene.h"

/* small3dlib config - must precede its include. */
#define S3L_PIXEL_FUNCTION     shade_pixel
#define S3L_RESOLUTION_X       GFX_WIDTH
#define S3L_RESOLUTION_Y       GFX_HEIGHT
#define S3L_Z_BUFFER           0  /* no depth buffer; sorting handles it */
#define S3L_SORT               1  /* back-to-front (painter's algorithm) */
#define S3L_MAX_TRIANGES_DRAWN 16 /* the cube has 12 */
#define S3L_SCISSOR_Y          1  /* band mode scissors S3L_drawTriangle() to one band's rows */
#include "small3dlib.h"

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

/* This frame's drawn-pixel bounds, accumulated by shade_pixel() while
 * render_lab_partial_updates is on - reset to an empty range at the top of
 * cube_rasterize_frame(), widened by every covered pixel small3dlib
 * reports. */
static int frame_x0, frame_y0, frame_x1, frame_y1;

/* This frame's overall cube coverage - the union of every bin entry's own
 * extent, accumulated by cube_transform_and_bin() - and last frame's,
 * remembered so band mode can mark the union of where the cube WAS and
 * where it IS dirty: a band the cube left still needs erasing even though
 * nothing there overlaps this frame. */
static int cube_bbox_x0, cube_bbox_y0, cube_bbox_x1, cube_bbox_y1;
static bool cube_bbox_valid;
static render_lab_coverage_t last_coverage;

/* Set only while cube_rasterize_band() runs; NULL otherwise, when
 * shade_pixel() writes into gfx_framebuffer() as before. small3dlib
 * rasterizes the whole scene once per band, so this is how the callback
 * keeps only the rows the current band owns. */
static gfx_color_t* band_target;
static int band_row0, band_row1;

static inline uint8_t
clamp_to_byte(S3L_Unit v) {
    if (v < 0) {
        return 0;
    }
    if (v > 255) {
        return 255;
    }
    return (uint8_t)v;
}

/* Called by small3dlib for every pixel a triangle covers - the equivalent
 * of a fragment shader, running on the CPU. pixel->barycentric holds
 * three weights summing to S3L_F that say how close this pixel is to
 * each corner, so averaging corner colours with them produces a smooth
 * gradient: Gouraud shading. Writes straight into the framebuffer rather
 * than through gfx_pixel(): this runs tens of thousands of times per
 * frame, and coordinates are already guaranteed on-screen by the
 * rasterizer. */
static inline void
shade_pixel(S3L_PixelInfo* pixel) {
    const S3L_Index* corners = cube_triangles + pixel->triangleIndex * 3;
    const uint8_t* a = cube_corner_colors[corners[0]];
    const uint8_t* b = cube_corner_colors[corners[1]];
    const uint8_t* c = cube_corner_colors[corners[2]];

    const uint8_t r = clamp_to_byte(S3L_interpolateBarycentric(a[0], b[0], c[0], pixel->barycentric));
    const uint8_t g = clamp_to_byte(S3L_interpolateBarycentric(a[1], b[1], c[1], pixel->barycentric));
    const uint8_t bl = clamp_to_byte(S3L_interpolateBarycentric(a[2], b[2], c[2], pixel->barycentric));
    const gfx_color_t color = gfx_rgb(((uint32_t)r << 16) | ((uint32_t)g << 8) | bl);

    if (band_target != NULL) {
        if (pixel->y < band_row0 || pixel->y >= band_row1) {
            return; /* not this band's row - small3dlib drew the whole scene */
        }
        band_target[(pixel->y - band_row0) * GFX_WIDTH + pixel->x] = color;
        return;
    }

    gfx_framebuffer()[pixel->y * GFX_WIDTH + pixel->x] = color;

    /* Only tracked in render_lab_partial_updates mode - cube_rasterize_frame() is the
     * sole reader, and there is no reason to pay for it on every one of the
     * tens of thousands of pixels a frame otherwise covers. */
    if (render_lab_partial_updates) {
        if (pixel->x < frame_x0) {
            frame_x0 = pixel->x;
        }
        if (pixel->x + 1 > frame_x1) {
            frame_x1 = pixel->x + 1;
        }
        if (pixel->y < frame_y0) {
            frame_y0 = pixel->y;
        }
        if (pixel->y + 1 > frame_y1) {
            frame_y1 = pixel->y + 1;
        }
    }
}

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

/* Exposed (suite_cube_perf.c) so the perf suite can time this without
 * touching small3dlib itself. small3dlib.h defines real, non-static
 * functions when included with S3L_PIXEL_FUNCTION etc. set - so only this
 * translation unit can call S3L_newFrame()/S3L_drawScene() at all; a
 * second #include from suite_cube_perf.c would redefine those symbols and
 * fail to link. */
void
cube_rasterize_frame(void) {
    if (render_lab_partial_updates) {
        frame_x0 = GFX_WIDTH;
        frame_y0 = GFX_HEIGHT;
        frame_x1 = 0;
        frame_y1 = 0;
    }

    /* S3L_SCISSOR_Y is on for this whole translation unit (band mode needs
     * it), so a full-fb frame must reset the range itself rather than trust
     * whatever band mode's own last band left behind - see
     * cube_rasterize_band()'s own comment. */
    S3L_scissorMinY = 0;
    S3L_scissorMaxY = GFX_HEIGHT;

    S3L_newFrame();       /* resets the triangle sorter */
    S3L_drawScene(scene); /* calls shade_pixel() for every covered pixel */

    if (render_lab_partial_updates) {
        /* shade_pixel() wrote straight into gfx_framebuffer(), which gfx
         * cannot see - this is the one gfx_mark_dirty() call that tells it
         * what actually changed this frame. */
        if (frame_x1 > frame_x0 && frame_y1 > frame_y0) {
            gfx_mark_dirty(frame_x0, frame_y0, frame_x1 - frame_x0, frame_y1 - frame_y0);
        }
    }
}

/* One visible triangle, transformed once per frame - see
 * cube_transform_and_bin(). y0/y1 is its screen-space row extent, so a band
 * can test overlap without touching small3dlib; sort_value is
 * S3L_drawScene()'s own depth key, kept so the bin stays back-to-front. */
typedef struct {
    S3L_Vec4 v0, v1, v2;
    S3L_Index triangle_index;
    int y0, y1;
    S3L_Unit sort_value;
} cube_triangle_bin_t;

static cube_triangle_bin_t cube_bin[S3L_CUBE_TRIANGLE_COUNT];
static int cube_bin_count;

/* Transforms and depth-sorts every visible triangle once per frame, so band
 * mode does not re-transform the whole scene once per band. Only correct
 * while S3L_NEAR_CROSS_STRATEGY stays 0: _S3L_projectTriangle() then never
 * splits a triangle across the near plane (asserted below), so one bin
 * entry per source triangle is enough. */
void
cube_transform_and_bin(void) {
    S3L_Mat4 mat_camera, mat_final;

    assert(cube.customTransformMatrix == 0); /* S3L_sceneInit()'s own default - never set by this scene */

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

        int x0 = transformed[0].x;
        int x1 = transformed[0].x;
        int y0 = transformed[0].y;
        int y1 = transformed[0].y;
        for (int i = 1; i < 3; i++) {
            const S3L_Unit x = transformed[i].x;
            const S3L_Unit y = transformed[i].y;
            if (x < x0) {
                x0 = x;
            }
            if (x > x1) {
                x1 = x;
            }
            if (y < y0) {
                y0 = y;
            }
            if (y > y1) {
                y1 = y;
            }
        }
        x0 = x0 < 0 ? 0 : x0;
        x1 = (x1 + 1 > GFX_WIDTH) ? GFX_WIDTH : x1 + 1;

        cube_triangle_bin_t entry;
        entry.v0 = transformed[0];
        entry.v1 = transformed[1];
        entry.v2 = transformed[2];
        entry.triangle_index = t;
        entry.y0 = y0 < 0 ? 0 : y0;
        entry.y1 = (y1 + 1 > GFX_HEIGHT) ? GFX_HEIGHT : y1 + 1; /* +1: inclusive of the bottom row */
        entry.sort_value = S3L_zeroClamp(transformed[0].w + transformed[1].w + transformed[2].w) >> 2;

        /* Insertion sort into place - the same shape as S3L_drawScene()'s
         * own sort, descending by sort_value (S3L_SORT == 1) so farther
         * triangles land first and nearer ones draw over them. */
        int slot = cube_bin_count;
        while (slot > 0 && cube_bin[slot - 1].sort_value < entry.sort_value) {
            cube_bin[slot] = cube_bin[slot - 1];
            slot--;
        }
        cube_bin[slot] = entry;
        cube_bin_count++;

        if (!cube_bbox_valid) {
            cube_bbox_x0 = x0;
            cube_bbox_y0 = entry.y0;
            cube_bbox_x1 = x1;
            cube_bbox_y1 = entry.y1;
            cube_bbox_valid = true;
        } else {
            if (x0 < cube_bbox_x0) {
                cube_bbox_x0 = x0;
            }
            if (entry.y0 < cube_bbox_y0) {
                cube_bbox_y0 = entry.y0;
            }
            if (x1 > cube_bbox_x1) {
                cube_bbox_x1 = x1;
            }
            if (entry.y1 > cube_bbox_y1) {
                cube_bbox_y1 = entry.y1;
            }
        }
    }

    /* Marked here, not by each caller: a band the cube left still needs
     * erasing even though nothing there overlaps this frame's own bbox,
     * and every band-mode caller of this function needs both boxes marked
     * the same way. */
    render_lab_coverage_mark(&last_coverage, cube_bbox_valid, cube_bbox_x0, cube_bbox_y0, cube_bbox_x1, cube_bbox_y1);
}

/* Draws only the bin's triangles that overlap [row0, row1) into `buf`,
 * scissored to those rows by S3L_SCISSOR_Y (small3dlib.h) - a triangle
 * confined to one band costs nothing in any other band, and even a
 * triangle spanning the whole screen only ever computes one band's worth
 * of rows per call. */
void
cube_rasterize_band(gfx_color_t* buf, int row0, int row1) {
    band_target = buf;
    band_row0 = row0;
    band_row1 = row1;
    S3L_scissorMinY = row0;
    S3L_scissorMaxY = row1;

    S3L_newFrame();
    for (int i = 0; i < cube_bin_count; i++) {
        const cube_triangle_bin_t* entry = &cube_bin[i];
        if (entry->y1 <= row0 || entry->y0 >= row1) {
            continue; /* this band's rows are entirely outside the triangle */
        }
        S3L_drawTriangle(entry->v0, entry->v1, entry->v2, 0, entry->triangle_index);
    }

    band_target = NULL;
}

static void
scene_cube_enter(void) {
    S3L_model3DInit(cube_vertices, S3L_CUBE_VERTEX_COUNT, cube_triangles, S3L_CUBE_TRIANGLE_COUNT, &cube);
    cube.transform.translation.z = CUBE_DISTANCE;

    /* S3L_sceneInit() resets the camera to defaults, so the focal length
     * override has to come after it. */
    S3L_sceneInit(&cube, 1, &scene);

    /* Enlarge by zooming rather than moving the cube closer: at this distance
     * the near plane (S3L_F/4) would clip the front faces long before the cube
     * filled the screen, and small3dlib discards triangles that cross it, so
     * faces would silently vanish. */
    scene.camera.focalLength = CAMERA_FOCAL_LENGTH;

    elapsed_ms = 0;
    last_coverage.valid = false; /* a stale box from a previous visit is not really "last frame" */
}

static void
scene_cube_frame(uint32_t dt_ms, bool band_mode_active) {
    cube_update_rotation(dt_ms);
    if (band_mode_active) {
        cube_transform_and_bin(); /* also marks the cube's own coverage dirty - see its own comment */
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
    /* Nothing allocated by scene_cube_enter() beyond static storage. */
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
