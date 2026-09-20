/*
 * app_render_lab - the software-rendering testbed launcher app.
 *
 * Draws into the shared framebuffer when the shell calls frame(), and
 * returns - it owns no frame loop, framebuffer or panel access of its own.
 *
 * Owns gfx_mode_enter()/exit(), the layout switch, BOOT handling, the menu,
 * the scene picker, the fps counter and the band loop. A scene
 * (render_lab_scene.h) owns only its own geometry, pose, clear colour and
 * coverage/dirty marking.
 */

#include <stdint.h>

#include "../../app.h"
#include "../../gfx/gfx.h"
#include "../../ui/ui.h"
#include "render_lab_mode_switch.h"
#include "render_lab_scene.h"
#include "ui/render_lab_hud_screen.h"
#include "ui/render_lab_menu_screen.h"

#define BACKGROUND_RGB 0x0A0C14

extern const render_lab_scene_t scene_cube;
extern const render_lab_scene_t scene_wire_plane;
extern const render_lab_scene_t scene_wire_cube;
extern const render_lab_scene_t scene_wire_sphere;
extern const render_lab_scene_t scene_wire_capsule;
extern bool partial_updates; /* scene_cube.c's toggle - this menu's own concern is only offering it */

static const render_lab_scene_t* const scenes[] = {
    &scene_cube, &scene_wire_plane, &scene_wire_cube, &scene_wire_sphere, &scene_wire_capsule,
};
#define SCENE_COUNT ((int)(sizeof(scenes) / sizeof(scenes[0])))
static int current_scene_index;

/* current_scene_index's own re-entry seed - 0 (the cube) by default. Set
 * directly by wire_render_host.c to start on a chosen scene with no menu
 * interaction, and kept in sync by the scene-cycle button below so a normal
 * re-entry still resumes wherever the user left it. Read only at enter(),
 * the same contract render_lab_band_mode below documents. */
int render_lab_start_scene_index;

static const render_lab_scene_t*
current_scene(void) {
    return scenes[current_scene_index];
}

/* Requests gfx's internal-SRAM band ring (GFX_LAYOUT_BANDS, gfx.h) instead
 * of the PSRAM framebuffer. On by default; runtime override for
 * suite_cube_band_perf.c. Read only at enter(), so flipping it mid-visit
 * needs a re-entry to take hold. */
bool render_lab_band_mode = true;

/* -1 (default): draw_overlay_box() centers the fps box as normal, same as
 * ever. Any other value pins the box's own logical x there instead - a
 * test-only hook (suite_cube_band_perf.c) for measuring the UI cost of a
 * box whose PANEL row extent (a 90-degree turn maps logical x onto panel
 * rows) starts on a band boundary rather than wherever centering lands
 * it, without touching the app's own default layout. */
int render_lab_fps_box_x_override = -1;

/* Whether the BOOT-opened menu is showing instead of the current scene -
 * the normal view renders only the scene and the fps counter, everything
 * else lives behind BOOT, the same one-button-one-screen-level-concern
 * split app_diagnostics.c and app_sand.c already use. */
static bool menu_open;
static render_lab_mode_switch_t mode_switch;

/* What gfx actually granted at enter() - not simply render_lab_band_mode,
 * which is only the request: gfx falls back to GFX_LAYOUT_FULL_FB if the
 * band ring fails to allocate, and render_lab_frame() has to follow the
 * grant rather than call gfx_band_*() against buffers that were never
 * allocated. */
static bool band_mode_active;

/* On-screen framerate readout - main.c's own report_fps() only ever reaches
 * a serial console, so this is what lets a scene's own cost be seen with
 * nothing but the board itself. Windowed on dt_ms rather than
 * esp_timer_get_time() like report_fps() does, so this needs nothing beyond
 * what render_lab_frame() is already handed. */
#define FPS_WINDOW_MS 500
static uint32_t fps_frame_count;
static uint32_t fps_window_elapsed_ms;
static double fps_value;

/* Last ui_layout_generation() seen, so render_lab_frame() can tell a shell
 * orientation change happened since last frame - see its own comment for
 * why that forces a full clear rather than a partial one. */
static uint32_t last_layout_generation;

static void
enter_layout(void) {
    const gfx_mode_request_t mode_request = {
        .layout = render_lab_band_mode ? GFX_LAYOUT_BANDS : GFX_LAYOUT_FULL_FB,
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
render_lab_enter(void) {
    enter_layout();
    current_scene_index = render_lab_start_scene_index;
    current_scene()->enter();

    fps_frame_count = 0;
    fps_window_elapsed_ms = 0;
    fps_value = 0.0;

    /* Always re-enter on the scene view, menu closed (never left open from a
     * previous visit, the same reason app_diagnostics.c resets `page` to 0
     * here), and with a fresh orientation baseline so a rotation that
     * happened while some OTHER app was showing does not read as "changed
     * since last frame" on the very first frame back here. */
    menu_open = false;
    mode_switch.pending = false;
    last_layout_generation = ui_layout_generation();
}

static void
switch_layout(void) {
    gfx_set_partial_clear(false);
    gfx_mode_exit();
    enter_layout();
    ui_invalidate();
}

/* The persistent HUD: the scene and, over it, the fps line - nothing else
 * renders while the menu is closed (see render_lab_frame()). Exposed for
 * performance testing (suite_cube_perf.c), timed as its own phase there.
 * `for_bands` builds the same commands either way; only the finishing
 * call differs - see ui_end_for_bands()'s own comment (ui.h). */
void
draw_fps(const input_t* input, bool for_bands) {
    mu_Context* ctx = ui_context();
    ui_begin(input);

    const render_lab_hud_screen_state_t state = {
        .fps_value = fps_value,
        .fps_box_x_override = render_lab_fps_box_x_override,
        .scene_name = current_scene()->name,
        .status = current_scene()->status != NULL ? current_scene()->status() : NULL,
    };
    render_lab_hud_screen_draw(ctx, &state);

    /* UI_NO_BACKGROUND is what lets the spinning scene show through
     * everywhere this window doesn't itself paint - see app_sand.c's
     * draw_palette() for the precedent. Unlike that panel's frozen sand,
     * the scene keeps moving underneath every frame, which is exactly the
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

/* The BOOT-opened menu holds the runtime rendering options and the scene
 * picker as centered bezel buttons - the place any future option belongs,
 * rather than growing the persistent HUD in draw_fps(). See menu_open's
 * own comment for why BOOT opens this instead of flipping a toggle
 * directly. */
static void
draw_menu(const input_t* input, bool for_bands, uint32_t dt_ms) {
    mu_Context* ctx = ui_context();
    ui_begin(input);

    const render_lab_menu_screen_state_t state = {
        .partial_updates_on = partial_updates,
        .band_mode_on = render_lab_band_mode,
        .scene_name = current_scene()->name,
    };
    const render_lab_menu_screen_result_t result = render_lab_menu_screen_draw(ctx, &state, dt_ms);

    if (result.partial_updates_clicked) {
        partial_updates = !partial_updates;

        /* Same reason render_lab_frame()'s BOOT handling forces this on
         * every open/close of this menu: flipping the toggle mid-visit
         * resets the partial clear cache. */
        gfx_invalidate();
    }
    if (result.band_mode_clicked) {
        render_lab_band_mode = !render_lab_band_mode;
        render_lab_mode_switch_request(&mode_switch);
    }
    if (result.next_scene_clicked) {
        current_scene()->exit();
        current_scene_index = (current_scene_index + 1) % SCENE_COUNT;
        render_lab_start_scene_index = current_scene_index; /* keeps a later re-entry on this same scene */
        current_scene()->enter();
        gfx_set_partial_clear(false);
        gfx_invalidate();
        ui_invalidate();
    }

    /* Modeled on app_sand.c's own draw_menu(): one full-screen OPAQUE
     * window (BACKGROUND_RGB, not UI_NO_BACKGROUND), because
     * render_lab_frame() does not draw the scene at all while menu_open is
     * true. In band mode render_lab_clear_band() already filled the whole
     * band with this same colour, so the finishing call there passes
     * UI_NO_BACKGROUND instead of paying for that fill twice. */
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

/* Fills an entire band buffer with the background colour - used only for
 * the menu's own backdrop in band mode; a scene's frame_band() clears its
 * own share as part of drawing it. Two pixels per store, the same trick
 * gfx_clear() uses. */
static void
render_lab_clear_band(gfx_color_t* buf, int height) {
    const gfx_color_t color = gfx_rgb(BACKGROUND_RGB);
    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t* words = (uint32_t*)buf;
    const int count = (GFX_WIDTH * height) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
    }
}

/* The band-mode frame: the fps counter and BOOT menu are built once
 * (for_bands=true) before the band loop and replayed into each band by
 * ui_replay_band() - ui.c's own general mechanism. menu_open skips the
 * scene entirely, matching render_lab_frame()'s full-fb shape. */
static void
render_lab_frame_band(uint32_t dt_ms, const input_t* input) {
    if (!menu_open) {
        update_fps_counter(dt_ms);
        current_scene()->frame(dt_ms, true);
    }

    if (menu_open) {
        draw_menu(input, true, dt_ms);
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
        if (menu_open) {
            render_lab_clear_band(buf, height);
        } else {
            current_scene()->frame_band(buf, row0, row0 + height);
        }
        ui_replay_band(row0, row0 + height);
        gfx_band_submit();
    }
}

static void
render_lab_frame(uint32_t dt_ms, const input_t* input) {
    if (render_lab_mode_switch_take(&mode_switch)) {
        switch_layout();
    }

    /* BOOT opens/closes the menu, rather than flipping a toggle directly.
     * Invalidation on open and close resets partial clear tracking: opening
     * replaces the framebuffer with the menu's opaque screen, and closing
     * repaints the scene from scratch. */
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
        render_lab_frame_band(dt_ms, input);
        return;
    }

    /* Everything below is the scene view: the fps counter measures ITS
     * throughput specifically, so counting a frame that only ever drew the
     * menu would blend two unrelated numbers into one misleading reading. */
    if (menu_open) {
        draw_menu(input, false, dt_ms);
        return;
    }

    update_fps_counter(dt_ms);
    current_scene()->frame(dt_ms, false);
    draw_fps(input, false);
}

void
render_lab_exit(void) {
    current_scene()->exit();
    gfx_set_partial_clear(false);
    gfx_invalidate();
    gfx_mode_exit();
}

/* gfx_request_full_redraw()'s app half (app.h): delegates to the current
 * scene, which owns the band-mode coverage union gfx cannot see. */
static void
render_lab_invalidate(void) {
    current_scene()->invalidate();
}

/* Exported as the struct itself rather than a pointer to it, so the registry
 * in main.c can take its address in a static initializer. */
const app_t app_render_lab = {
    .name = "Render Lab",
    .summary = "Software rendering experiments",
    .enter = render_lab_enter,
    .frame = render_lab_frame,
    .exit = render_lab_exit,
    .invalidate = render_lab_invalidate,
    .home_gesture = true,
};

APP_REGISTER(app_render_lab);
