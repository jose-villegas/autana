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
#include "render_lab.h"
#include "render_lab_mode_switch.h"
#include "render_lab_scene.h"
#include "ui/render_lab_hud_screen.h"
#include "ui/render_lab_menu_screen.h"

extern const render_lab_scene_t scene_cube;
extern const render_lab_scene_t scene_wire_plane;
extern const render_lab_scene_t scene_wire_cube;
extern const render_lab_scene_t scene_wire_sphere;
extern const render_lab_scene_t scene_wire_capsule;
extern const render_lab_scene_t scene_raytrace;
bool render_lab_partial_updates = true;

static const render_lab_scene_t* const scenes[] = {
    &scene_cube, &scene_wire_plane, &scene_wire_cube, &scene_wire_sphere, &scene_wire_capsule, &scene_raytrace,
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

/* Hides the fps/title overlay draw_fps() builds - on by default. A render
 * host pin needs it off: the fps line is a double formatted with "%.1f",
 * which a pin cannot rely on across compilers. Read every frame. */
bool render_lab_show_hud = true;

/* -1 (default) leaves the fps box at its corner inset. Any other value pins
 * the box's own logical x there instead - a test-only hook
 * (suite_cube_band_perf.c) for measuring the UI cost of a box whose PANEL
 * row extent (a 90-degree turn maps logical x onto panel rows) starts on a
 * band boundary rather than wherever the inset lands it. */
int render_lab_fps_box_x_override = -1;

/* How long a scene's name stays on screen after the scene is entered. The
 * BOOT menu shows the name at any time, so the HUD does not keep it. */
#define SCENE_TITLE_MS      2000
#define SCENE_TITLE_FADE_MS 500
static uint32_t scene_title_remaining_ms;

static uint8_t
scene_title_alpha(void) {
    if (scene_title_remaining_ms >= SCENE_TITLE_FADE_MS) {
        return 255;
    }
    return (uint8_t)(scene_title_remaining_ms * 255 / SCENE_TITLE_FADE_MS);
}

/* Whether the BOOT-opened menu is showing instead of the current scene -
 * the normal view renders only the scene and the fps counter, everything
 * else lives behind BOOT, the same one-button-one-screen-level-concern
 * split app_diagnostics.c and app_sand.c already use. */
static bool menu_open;
static render_lab_mode_switch_t mode_switch;

/* A scene change can change the layout, and the menu that asks for one runs
 * mid-frame, with a band frame possibly about to begin - so it is taken at
 * the top of the next frame, as the layout toggle is. */
static bool scene_switch_pending;

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

/* current_scene_index must already name the scene about to run: a scene
 * with needs_full_framebuffer set (render_lab_scene.h) overrides
 * render_lab_band_mode, so the request depends on which scene this is. */
static void
enter_layout(void) {
    const bool bands = render_lab_band_mode && !current_scene()->needs_full_framebuffer;
    const gfx_mode_request_t mode_request = {
        .layout = bands ? GFX_LAYOUT_BANDS : GFX_LAYOUT_FULL_FB,
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
    current_scene_index = render_lab_start_scene_index;
    enter_layout();
    current_scene()->enter();
    scene_title_remaining_ms = SCENE_TITLE_MS;

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
    scene_switch_pending = false;
    last_layout_generation = ui_layout_generation();
}

static void
switch_layout(void) {
    gfx_set_partial_clear(false);
    gfx_mode_exit();
    enter_layout();
    ui_invalidate();
}

static void
switch_to_next_scene(void) {
    current_scene()->exit();
    current_scene_index = (current_scene_index + 1) % SCENE_COUNT;
    render_lab_start_scene_index = current_scene_index; /* keeps a later re-entry on this same scene */
    switch_layout(); /* the new scene's needs_full_framebuffer may differ from the old one's */
    current_scene()->enter();
    scene_title_remaining_ms = SCENE_TITLE_MS;
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
        .scene_title = current_scene()->name,
        .scene_title_alpha = scene_title_alpha(),
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
        .partial_updates_on = render_lab_partial_updates,
        .band_mode_on = render_lab_band_mode,
        .scene_name = current_scene()->name,
    };
    const render_lab_menu_screen_result_t result = render_lab_menu_screen_draw(ctx, &state, dt_ms);

    if (result.partial_updates_clicked) {
        render_lab_partial_updates = !render_lab_partial_updates;

        /* Same reason render_lab_frame()'s BOOT handling forces this on
         * every open/close of this menu: flipping the toggle mid-visit
         * resets the partial clear cache. */
        gfx_invalidate();
    }
    /* A scene with needs_full_framebuffer set overrides the request either
     * way, so toggling it here would only cost a layout re-entry with
     * nothing for the user to see - the button simply does nothing. */
    if (result.band_mode_clicked && !current_scene()->needs_full_framebuffer) {
        render_lab_band_mode = !render_lab_band_mode;
        render_lab_mode_switch_request(&mode_switch);
    }
    if (result.next_scene_clicked) {
        scene_switch_pending = true;
    }

    /* Modeled on app_sand.c's own draw_menu(): one full-screen OPAQUE
     * window (RENDER_LAB_BACKGROUND_RGB, not UI_NO_BACKGROUND), because
     * render_lab_frame() does not draw the scene at all while menu_open is
     * true. In band mode render_lab_clear_band() already filled the whole
     * band with this same colour, so the finishing call there passes
     * UI_NO_BACKGROUND instead of paying for that fill twice. */
    if (for_bands) {
        ui_end_for_bands(UI_NO_BACKGROUND);
    } else {
        ui_end(RENDER_LAB_BACKGROUND_RGB);
    }
}

/* fps_value only actually changes once a window closes, so it reads as a
 * settled average rather than jittering with every frame's own dt_ms -
 * same reason report_fps() in main.c windows instead of reporting per
 * frame. Shared by both render paths so the readout means the same thing
 * in either mode. */
/* No scene erases the title's box, so the frame it expires on is redrawn in
 * full. */
static void
update_scene_title(uint32_t dt_ms) {
    if (scene_title_remaining_ms == 0) {
        return;
    }
    scene_title_remaining_ms = scene_title_remaining_ms > dt_ms ? scene_title_remaining_ms - dt_ms : 0;
    if (scene_title_remaining_ms == 0) {
        gfx_invalidate();
        current_scene()->invalidate();
    }
}

static void
update_fps_counter(uint32_t dt_ms) {
    update_scene_title(dt_ms);
    fps_frame_count++;
    fps_window_elapsed_ms += dt_ms;
    if (fps_window_elapsed_ms >= FPS_WINDOW_MS) {
        fps_value = (double)fps_frame_count * 1000.0 / (double)fps_window_elapsed_ms;
        fps_frame_count = 0;
        fps_window_elapsed_ms = 0;
    }
}

/* Two pixels per store, the same trick gfx_clear() uses. */
void
render_lab_clear_band(gfx_color_t* buf, int height) {
    const gfx_color_t color = gfx_rgb(RENDER_LAB_BACKGROUND_RGB);
    const uint32_t pair = ((uint32_t)color << 16) | color;
    uint32_t* words = (uint32_t*)buf;
    const int count = (GFX_WIDTH * height) / 2;

    for (int i = 0; i < count; i++) {
        words[i] = pair;
    }
}

void
render_lab_coverage_mark(render_lab_coverage_t* last, bool have, int x0, int y0, int x1, int y1) {
    if (last->valid) {
        gfx_mark_dirty(last->x0, last->y0, last->x1 - last->x0, last->y1 - last->y0);
    }
    if (have) {
        gfx_mark_dirty(x0, y0, x1 - x0, y1 - y0);
        *last = (render_lab_coverage_t){x0, y0, x1, y1, true};
    }
    last->valid = have;
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
    } else if (render_lab_show_hud) {
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
        render_lab_clear_band(buf, height);
        if (!menu_open) {
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
    if (scene_switch_pending) {
        scene_switch_pending = false;
        switch_to_next_scene();
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
    if (render_lab_show_hud) {
        draw_fps(input, false);
    }
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
