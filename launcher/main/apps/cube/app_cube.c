/*
 * app_cube - the Gouraud-shaded rotating RGB cube, as a launcher app.
 *
 * Ownership is inverted compared with a standalone renderer: this does not run
 * a frame loop, own a framebuffer, or talk to the panel. It draws into the
 * shared framebuffer when the shell calls frame(), and returns.
 *
 * Why small3dlib rather than a conventional rasterizer: it owns no
 * framebuffer (every rasterized pixel comes back through a callback) and with
 * S3L_Z_BUFFER 0 it keeps no depth buffer, resolving visibility by sorting
 * triangles back-to-front. A colour+depth rasterizer would want ~1.3 MB at
 * this resolution, against ~424 KiB of RAM on the whole chip.
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../../app.h"
#include "../../gfx/gfx.h"
#include "../../ui/ui.h"
#include "cube_mode_switch.h"
#include "ui/cube_hud_screen.h"
#include "ui/cube_menu_screen.h"

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

#define BACKGROUND_RGB      0x0A0C14

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

/* The toggle this file exists to demonstrate: whether cube_frame() clears
 * the whole framebuffer every frame or only the pixels the cube touches,
 * via gfx_mark_dirty() instead of gfx_clear()'s implicit "everything
 * changed". On by default - the partial-clear path is what this app
 * showcases. Flipped from inside draw_menu(), not directly by BOOT any
 * more. Exposed so suite_cube_perf.c can force this true in its fixture,
 * so a stray leftover toggle can never silently skew a perf run. */
bool partial_updates = true;

/* Requests gfx's internal-SRAM band ring (GFX_LAYOUT_BANDS, gfx.h) instead
 * of the PSRAM framebuffer. On by default; runtime override for
 * suite_cube_band_perf.c. Read only at enter(), so flipping it mid-visit
 * needs a re-entry to take hold. */
bool cube_band_mode = true;

/* -1 (default): draw_overlay_box() centers the fps box as normal, same as
 * ever. Any other value pins the box's own logical x there instead - a
 * test-only hook (suite_cube_band_perf.c) for measuring the UI cost of a
 * box whose PANEL row extent (a 90-degree turn maps logical x onto panel
 * rows) starts on a band boundary rather than wherever centering lands
 * it, without touching the app's own default layout. */
int cube_fps_box_x_override = -1;

/* Whether the BOOT-opened menu (draw_menu()) is showing instead of the
 * cube. The normal view renders only the cube and the fps counter - see
 * cube_frame()'s own comment - and everything else, right now just the
 * partial_updates toggle, lives behind BOOT in here, the same
 * one-physical-button-one-screen-level-concern split app_diagnostics.c's
 * page cycling and app_sand.c's SAND_UI_MENU/RUNNING split already use. */
static bool menu_open;
static cube_mode_switch_t mode_switch;

/* This frame's drawn-pixel bounds, accumulated by shade_pixel() while
 * partial_updates is on - reset to an empty range at the top of
 * cube_frame(), widened by every covered pixel small3dlib reports. */
static int frame_x0, frame_y0, frame_x1, frame_y1;

/* This frame's overall cube coverage - the union of every bin entry's own
 * extent, accumulated by cube_transform_and_bin() - and last frame's,
 * remembered so band mode can mark the union of where the cube WAS and
 * where it IS dirty: a band the cube left still needs erasing even though
 * nothing there overlaps this frame. Declared ahead of cube_enter() below,
 * which resets prev_cube_bbox_valid on every visit. */
static int cube_bbox_x0, cube_bbox_y0, cube_bbox_x1, cube_bbox_y1;
static bool cube_bbox_valid;
static int prev_cube_bbox_x0, prev_cube_bbox_y0, prev_cube_bbox_x1, prev_cube_bbox_y1;
static bool prev_cube_bbox_valid;

/* Set only while cube_rasterize_band() runs; NULL otherwise, when
 * shade_pixel() writes into gfx_framebuffer() as before. small3dlib
 * rasterizes the whole scene once per band, so this is how the callback
 * keeps only the rows the current band owns. */
static gfx_color_t* band_target;
static int band_row0, band_row1;

/* What gfx actually granted at enter() - not simply cube_band_mode, which
 * is only the request: gfx falls back to GFX_LAYOUT_FULL_FB if the band
 * ring fails to allocate, and cube_frame() has to follow the grant rather
 * than call gfx_band_*() against buffers that were never allocated. */
static bool band_mode_active;

/* On-screen framerate readout - the other half of what makes the toggle
 * above worth having: main.c's own report_fps() only ever reaches a
 * serial console, so this is what lets partial_updates' effect be seen
 * with nothing but the board itself. Windowed on dt_ms rather than
 * esp_timer_get_time() like report_fps() does, so this needs nothing
 * beyond what cube_frame() is already handed. */
#define FPS_WINDOW_MS 500
static uint32_t fps_frame_count;
static uint32_t fps_window_elapsed_ms;
static double fps_value;

/* Last ui_layout_generation() seen, so cube_frame() can tell a shell
 * orientation change happened since last frame - see its own comment for
 * why that forces a full clear rather than a partial one. */
static uint32_t last_layout_generation;

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

    /* Only tracked in partial_updates mode - cube_frame() is the sole
     * reader, and there is no reason to pay for it on every one of the
     * tens of thousands of pixels a frame otherwise covers. cube_frame()
     * is also where the gfx_mark_dirty() call these bounds feed into
     * lives - see its own comment on why writing gfx_framebuffer()
     * directly, as this does, requires one. */
    if (partial_updates) {
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

static void
enter_layout(void) {
    const gfx_mode_request_t mode_request = {
        .layout = cube_band_mode ? GFX_LAYOUT_BANDS : GFX_LAYOUT_FULL_FB,
        .resolution = GFX_RESOLUTION_FULL,
        .interlace_x = false,
        .interlace_y = false,
    };
    band_mode_active = gfx_mode_enter(&mode_request)->layout == GFX_LAYOUT_BANDS;
    /* The framebuffer is whatever the previous app or layout left in it -
     * the first frame after entering, in either mode, has to clear in full.
     * gfx_invalidate() ensures the partial clear cache starts fresh. */
    gfx_invalidate();
}

void
cube_enter(void) {
    enter_layout();

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

    /* partial_updates itself is deliberately left alone: a developer
     * toggle that reset every visit would defeat the point of it, same
     * as show_orientation in app_diagnostics.c. The readout, unlike it,
     * starts over every visit - a stale fps_value left over from a previous
     * run would show a number with nothing behind it for up to FPS_WINDOW_MS. */
    fps_frame_count = 0;
    fps_window_elapsed_ms = 0;
    fps_value = 0.0;

    /* Always re-enter on the cube view, never with the menu left open from
     * a previous visit - the same reason app_diagnostics.c resets `page` to
     * 0 here instead of leaving it wherever a past visit left it. */
    menu_open = false;
    mode_switch.pending = false;

    /* A stale box from a previous visit is not really "last frame" -
     * gfx_invalidate() above already forces this visit's first band frame
     * regardless, so this only avoids marking a box nobody drew any more. */
    prev_cube_bbox_valid = false;

    /* Baseline for the orientation check in cube_frame() - without this,
     * a rotation that happened while some OTHER app was showing would
     * read as "changed since last frame" on the very first frame back in
     * the cube, forcing a clear that bbox_valid's own false above already
     * forces. Not wrong, just redundant with a clearer reason already
     * stated. */
    last_layout_generation = ui_layout_generation();
}

static void
switch_layout(void) {
    gfx_set_partial_clear(false);
    gfx_mode_exit();
    enter_layout();
    ui_invalidate();
}

/* The persistent HUD: the cube and, over it, the fps line - nothing else
 * renders while the menu is closed (see cube_frame()). Exposed for
 * performance testing (suite_cube_perf.c), timed as its own phase there.
 * `for_bands` builds the same commands either way; only the finishing
 * call differs - see ui_end_for_bands()'s own comment (ui.h). */
void
draw_fps(const input_t* input, bool for_bands) {
    mu_Context* ctx = ui_context();
    ui_begin(input);

    const cube_hud_screen_state_t state = {
        .fps_value = fps_value,
        .fps_box_x_override = cube_fps_box_x_override,
    };
    cube_hud_screen_draw(ctx, &state);

    /* UI_NO_BACKGROUND is what lets the spinning cube show through
     * everywhere this window doesn't itself paint - see app_sand.c's
     * draw_palette() for the precedent. Unlike that panel's frozen sand,
     * the cube keeps moving underneath every frame, which is exactly the
     * case ui_end()'s own comment calls out: it repaints whenever
     * "something else has already dirtied the screen", so the fps line
     * stays correctly composited over a background that never stops
     * changing, with no special handling needed here. */
    if (for_bands) {
        ui_end_for_bands(UI_NO_BACKGROUND);
    } else {
        ui_end(UI_NO_BACKGROUND);
    }
}

/* The BOOT-opened menu holds the runtime rendering options as centered
 * bezel buttons, but the place any future option belongs
 * rather than growing the persistent HUD in draw_fps(). See cube_frame()
 * for why BOOT opens this instead of flipping the toggle directly, and
 * menu_open's own comment for the one-button-one-screen-level-concern
 * precedent this follows. */
static void
draw_menu(const input_t* input, bool for_bands) {
    mu_Context* ctx = ui_context();
    ui_begin(input);

    const cube_menu_screen_state_t state = {
        .partial_updates_on = partial_updates,
        .band_mode_on = cube_band_mode,
    };
    const cube_menu_screen_result_t result = cube_menu_screen_draw(ctx, &state);

    if (result.partial_updates_clicked) {
        partial_updates = !partial_updates;

        /* Same reason cube_frame()'s BOOT handling forces this on every
         * open/close of this menu: flipping the toggle mid-visit resets
         * the partial clear cache. */
        gfx_invalidate();
    }
    if (result.band_mode_clicked) {
        cube_band_mode = !cube_band_mode;
        cube_mode_switch_request(&mode_switch);
    }

    /* Modeled on app_sand.c's own draw_menu(): one full-screen OPAQUE
     * window (BACKGROUND_RGB, not UI_NO_BACKGROUND), because cube_frame()
     * does not draw the cube at all while menu_open is true. In band mode
     * clear_band() already filled the whole band with this same colour,
     * so the finishing call there passes UI_NO_BACKGROUND instead of
     * paying for that fill twice. */
    if (for_bands) {
        ui_end_for_bands(UI_NO_BACKGROUND);
    } else {
        ui_end(BACKGROUND_RGB);
    }
}

/* fps_value only actually changes once a window closes, so it reads as a
 * settled average rather than jittering with every frame's own dt_ms -
 * same reason report_fps() in main.c windows instead of reporting per
 * frame. Shared by both render paths so the readout means the same thing
 * in either mode. */
static void
update_fps_counter(uint32_t dt_ms) {
    fps_frame_count++;
    fps_window_elapsed_ms += dt_ms;
    if (fps_window_elapsed_ms >= FPS_WINDOW_MS) {
        fps_value = (double)fps_frame_count * 1000.0 / (double)fps_window_elapsed_ms;
        fps_frame_count = 0;
        fps_window_elapsed_ms = 0;
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
    gfx_set_partial_clear(partial_updates);
    gfx_clear(gfx_rgb(BACKGROUND_RGB));
}

/* The three phases below are exposed (suite_cube_perf.c) so the perf
 * suite can time each without touching small3dlib itself. small3dlib.h
 * defines real, non-static functions when included with
 * S3L_PIXEL_FUNCTION etc. set - so only this translation unit can call
 * S3L_newFrame()/S3L_drawScene() at all; a second #include from
 * suite_cube_perf.c would redefine those symbols and fail to link.
 * cube_frame() is just these three calls plus draw_fps(), exercising the
 * exact same code. */
void
cube_rasterize_frame(void) {
    if (partial_updates) {
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

    if (partial_updates) {
        /* shade_pixel() wrote straight into gfx_framebuffer(), which gfx
         * cannot see - this is the one gfx_mark_dirty() call that tells it
         * what actually changed this frame. */
        if (frame_x1 > frame_x0 && frame_y1 > frame_y0) {
            gfx_mark_dirty(frame_x0, frame_y0, frame_x1 - frame_x0, frame_y1 - frame_y0);
        }
    }
}

/* Fills an entire band buffer with the background colour - the band ring
 * has no accumulated framebuffer to clear a bounding box out of, so every
 * band is a full redraw regardless of partial_updates: gfx_clear()'s own
 * partial path never fires without gfx_present()'s bookkeeping, which
 * band mode's no-op present (gfx.c) never runs. Two pixels per store, the
 * same trick cube_clear_frame()'s own gfx_clear() uses. */
static void
clear_band(gfx_color_t* buf, int height) {
    const gfx_color_t color = gfx_rgb(BACKGROUND_RGB);
    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t* words = (uint32_t*)buf;
    const int count = (GFX_WIDTH * height) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
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

    assert(cube.customTransformMatrix == 0); /* S3L_sceneInit()'s own default - never set by this app */

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
    if (prev_cube_bbox_valid) {
        gfx_mark_dirty(prev_cube_bbox_x0, prev_cube_bbox_y0, prev_cube_bbox_x1 - prev_cube_bbox_x0,
                       prev_cube_bbox_y1 - prev_cube_bbox_y0);
    }
    if (cube_bbox_valid) {
        gfx_mark_dirty(cube_bbox_x0, cube_bbox_y0, cube_bbox_x1 - cube_bbox_x0, cube_bbox_y1 - cube_bbox_y0);
    }
    prev_cube_bbox_x0 = cube_bbox_x0;
    prev_cube_bbox_y0 = cube_bbox_y0;
    prev_cube_bbox_x1 = cube_bbox_x1;
    prev_cube_bbox_y1 = cube_bbox_y1;
    prev_cube_bbox_valid = cube_bbox_valid;
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

/* The band-mode frame: the fps counter and BOOT menu are built once
 * (for_bands=true) before the band loop and replayed into each band by
 * ui_replay_band() - ui.c's own general mechanism, not built for this app
 * alone. menu_open skips the cube entirely, matching cube_frame()'s
 * full-fb shape. */
static void
cube_frame_band(uint32_t dt_ms, const input_t* input) {
    if (!menu_open) {
        update_fps_counter(dt_ms);
        cube_update_rotation(dt_ms);
        cube_transform_and_bin(); /* also marks the cube's own coverage dirty - see its own comment */
    }

    if (menu_open) {
        draw_menu(input, true);
    } else {
        draw_fps(input, true);
    }

    gfx_band_frame_begin();
    while (gfx_band_next()) {
        const int row0 = gfx_band_row0();
        const int height = gfx_band_height();

        /* touched_x0/x1 (the column span worth touching) is not narrowed
         * further yet - the whole band's own internal-SRAM buffer is
         * reused across bands, so sending less than the whole width would
         * need packing the same way gfx.c's own gather_and_send() does for
         * full-fb, which is future work; only whether to touch the band
         * at all is exploited here. */
        int touched_x0, touched_x1;
        if (!gfx_band_dirty(row0, row0 + height, &touched_x0, &touched_x1)) {
            gfx_band_skip(); /* the panel already shows what belongs here */
            continue;
        }
        (void)touched_x0;
        (void)touched_x1;

        gfx_color_t* buf = gfx_band_buffer();
        clear_band(buf, height);
        if (!menu_open) {
            cube_rasterize_band(buf, row0, row0 + height);
        }
        ui_replay_band(row0, row0 + height);
        gfx_band_submit();
    }
}

static void
cube_frame(uint32_t dt_ms, const input_t* input) {
    if (cube_mode_switch_take(&mode_switch)) {
        switch_layout();
    }

    /* BOOT opens/closes the menu now, rather than flipping partial_updates
     * directly - the toggle moved onto its own bezel button inside
     * draw_menu(). Invalidation on open and close resets partial clear
     * tracking for the same reasons: opening replaces the framebuffer
     * with the menu's opaque screen, and closing repaints the cube from
     * scratch. Shared by both render paths: BOOT must open the menu
     * whichever one is running. */
    if (input->boot.pressed) {
        menu_open = !menu_open;
        gfx_invalidate();

        if (menu_open) {
            ui_invalidate();
        }
    }

    /* A shell orientation change moves draw_fps()'s overlay to a different
     * physical region - gfx_invalidate() ensures the next frame performs a
     * full screen wipe rather than a partial one. */
    const uint32_t layout_generation = ui_layout_generation();
    if (layout_generation != last_layout_generation) {
        last_layout_generation = layout_generation;
        gfx_invalidate();
    }

    if (band_mode_active) {
        cube_frame_band(dt_ms, input);
        return;
    }

    /* Everything below is the cube view: the fps counter measures ITS
     * throughput specifically, so counting a frame that only ever drew the
     * menu would blend two unrelated numbers into one misleading reading. */
    if (menu_open) {
        draw_menu(input, false);
        return;
    }

    update_fps_counter(dt_ms);
    cube_update_rotation(dt_ms);
    cube_clear_frame();
    cube_rasterize_frame();
    draw_fps(input, false);
}

void
cube_exit(void) {
    gfx_set_partial_clear(false);
    gfx_invalidate();
    gfx_mode_exit();
}

/* gfx_request_full_redraw()'s app half (app.h): the band-mode coverage
 * union gfx cannot see - a stale box from before the request would mark a
 * region nobody is about to redraw, the same reasoning cube_enter() gives
 * for resetting it there. */
static void
cube_invalidate(void) {
    prev_cube_bbox_valid = false;
}

/* Exported as the struct itself rather than a pointer to it, so the registry
 * in main.c can take its address in a static initializer. */
const app_t app_cube = {
    .name = "3D Cube",
    .summary = "Gouraud-shaded software rasterizer",
    .enter = cube_enter,
    .frame = cube_frame,
    .exit = cube_exit,
    .invalidate = cube_invalidate,
    .home_gesture = true,
};

APP_REGISTER(app_cube);
