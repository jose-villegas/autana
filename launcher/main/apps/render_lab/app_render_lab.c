/* app_render_lab: mesh scenes behind a shared HUD and scene picker. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "ui/render_lab_hud_screen.h"
#include "ui/render_lab_menu_screen.h"
#include "ui/ui.h"
#include "util/build/build_variant.h"
#include "util/runtime/tune.h"

extern const render_lab_scene_t scene_sponza;
extern const render_lab_scene_t scene_capybara;
extern const render_lab_scene_t scene_sponza_lite;
extern const render_lab_scene_t scene_sponza_flat;
extern const render_lab_scene_t scene_sponza_fitted;
extern const render_lab_scene_t scene_sponza_fitted_full;
extern const render_lab_scene_t scene_sponza_flat_fitted;

static const render_lab_scene_t* const scenes[] = {
    &scene_sponza,        &scene_sponza_lite,        &scene_sponza_flat,
    &scene_sponza_fitted, &scene_sponza_fitted_full, &scene_sponza_flat_fitted,
    &scene_capybara,
};
#define SCENE_COUNT ((int)(sizeof(scenes) / sizeof(scenes[0])))
static int current_scene_index;

const char* render_lab_start_scene_key;
const char* render_lab_start_camera;

/* Unknown or unset resolves to the first scene, never a hard error. */
static int
scene_index_for_key(const char* key) {
    if (key != NULL) {
        for (int i = 0; i < SCENE_COUNT; i++) {
            if (strcmp(scenes[i]->key, key) == 0) {
                return i;
            }
        }
    }
    return 0;
}

static const render_lab_scene_t*
current_scene(void) {
    return scenes[current_scene_index];
}

/* Hides the fps/title overlay draw_fps() builds, on by default. A render
 * host pin needs it off: the fps line is a double formatted with "%.1f",
 * which a pin cannot rely on across compilers. Read every frame. */
bool render_lab_show_hud = true;

TUNE_OWNER(render_lab);
TUNE(render_lab, view, RENDER_VIEW_SHADED, RENDER_VIEW_SHADED, RENDER_VIEW_COUNT);
TUNE(render_lab, scale, 200, 100, 800);
TUNE(render_lab, budget, 0, 0, 200);

int
render_lab_view(void) {
    return (int)view;
}

int
render_lab_scale(void) {
    return scale;
}

int
render_lab_budget_ms(void) {
    return budget;
}

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
 * else lives behind BOOT. */
static bool menu_open;

static bool scene_switch_pending;

/* A scene the console asked for by key, switched to at the next frame like
 * a menu tap; -1 for none. */
static int scene_requested = -1;

/* On-screen framerate, since the shell's report_fps() reaches only the
 * serial console. Windowed on the dt_ms render_lab_frame() is handed. */
#define FPS_WINDOW_MS 500
static uint32_t fps_frame_count;
static uint32_t fps_window_elapsed_ms;
static float fps_value;

/* Orientation changes invalidate the HUD layout. */
static uint32_t last_layout_generation;

static void
enter_layout(void) {
    const gfx_mode_request_t mode_request = {
        .layout = GFX_LAYOUT_FULL_FB,

        .interlace_x = false,
        .interlace_y = false,
    };
    (void)gfx_mode_enter(&mode_request);

    gfx_invalidate();
}

void
render_lab_enter(void) {
    current_scene_index = scene_index_for_key(render_lab_start_scene_key);
    enter_layout();
    current_scene()->enter();
    scene_title_remaining_ms = SCENE_TITLE_MS;

    fps_frame_count = 0;
    fps_window_elapsed_ms = 0;
    fps_value = 0.0F;

    /* Always re-enter on the scene view, menu closed (never left open from a
     * previous visit), and with a fresh orientation baseline so a rotation that
     * happened while some OTHER app was showing does not read as "changed
     * since last frame" on the very first frame back here. */
    menu_open = false;
    scene_switch_pending = false;
    scene_requested = -1;
    last_layout_generation = ui_layout_generation();
}

static void
switch_to_scene(int index) {
    current_scene()->exit();
    current_scene_index = index;
    render_lab_start_scene_key = current_scene()->key; /* keeps a later re-entry on this same scene */
    gfx_invalidate();
    ui_invalidate();
    current_scene()->enter();
    scene_title_remaining_ms = SCENE_TITLE_MS;
}

static void
switch_to_next_scene(void) {
    switch_to_scene((current_scene_index + 1) % SCENE_COUNT);
}

static void
draw_fps(const input_t* input) {
    mu_Context* ctx = ui_context();
    ui_begin(input);

    const render_lab_hud_screen_state_t state = {
        .fps_value = fps_value,
        .scene_title = current_scene()->name,
        .scene_title_alpha = scene_title_alpha(),
        .status = current_scene()->status != NULL ? current_scene()->status() : NULL,
    };
    render_lab_hud_screen_draw(ctx, &state);

    /* The scene changes underneath the HUD every frame, so the overlay
     * must composite over the current scene without an opaque window. */
    ui_end(UI_NO_BACKGROUND);
}

static void
draw_menu(const input_t* input, uint32_t dt_ms) {
    mu_Context* ctx = ui_context();
    ui_begin(input);

    const render_lab_menu_screen_state_t state = {
        .scene_name = current_scene()->name,
    };
    const render_lab_menu_screen_result_t result = render_lab_menu_screen_draw(ctx, &state, dt_ms);

    if (result.next_scene_clicked) {
        scene_switch_pending = true;
    }

    ui_end(RENDER_LAB_BACKGROUND_RGB);
}

static void
update_scene_title(uint32_t dt_ms) {
    if (scene_title_remaining_ms == 0) {
        return;
    }
    scene_title_remaining_ms = scene_title_remaining_ms > dt_ms ? scene_title_remaining_ms - dt_ms : 0;
    if (scene_title_remaining_ms == 0) {
        gfx_invalidate();
    }
}

static void
update_fps_counter(uint32_t dt_ms) {
    update_scene_title(dt_ms);
    fps_frame_count++;
    fps_window_elapsed_ms += dt_ms;
    if (fps_window_elapsed_ms >= FPS_WINDOW_MS) {
        fps_value = (float)fps_frame_count * 1000.0F / (float)fps_window_elapsed_ms;
        fps_frame_count = 0;
        fps_window_elapsed_ms = 0;
    }
}

static void
render_lab_frame(uint32_t dt_ms, const input_t* input) {
    if (scene_switch_pending) {
        scene_switch_pending = false;
        switch_to_next_scene();
    }
    if (scene_requested >= 0) {
        if (scene_requested != current_scene_index) {
            switch_to_scene(scene_requested);
        }
        scene_requested = -1;
    }

    if (input->boot.pressed) {
        menu_open = !menu_open;
        scene_set_paused(menu_open);
        gfx_invalidate();

        if (menu_open) {
            ui_invalidate();
        }
    }

    const uint32_t layout_generation = ui_layout_generation();
    if (layout_generation != last_layout_generation) {
        last_layout_generation = layout_generation;
        gfx_invalidate();
    }

    /* Everything below is the scene view: the fps counter measures ITS
     * throughput specifically, so counting a frame that only ever drew the
     * menu would blend two unrelated numbers into one misleading reading. */
    if (menu_open) {
        draw_menu(input, dt_ms);
        return;
    }

    update_fps_counter(dt_ms);
    current_scene()->frame(dt_ms);
    if (render_lab_show_hud) {
        draw_fps(input);
    }
}

void
render_lab_exit(void) {
    current_scene()->exit();
    gfx_invalidate();
    gfx_mode_exit();
}

/* The scene's own steer() and update(), where it has them, overlapped with
 * the send of the frame drawn last pass. A scene switch or the menu takes effect in frame(),
 * which runs after this, so a scene must cope with frame() arriving without
 * a matching update(). */
static void
render_lab_update(uint32_t dt_ms, const input_t* input) {
    if (menu_open) {
        return;
    }
    if (current_scene()->steer != NULL) {
        current_scene()->steer(dt_ms, input);
    }
    if (current_scene()->update != NULL) {
        current_scene()->update(dt_ms);
    }
}

/* "render scenes" lists every scene's key and name and which one shows;
 * "render scene <key>" switches to the scene with exactly that key, at the
 * next frame. Replies are "RENDER ..." lines then "RENDER_END", or
 * "RENDER_ERR <why>", so a script can measure one scene alike on any build. */
static bool
render_lab_console(const char* args) {
    if (strcmp(args, "scenes") == 0) {
        for (int i = 0; i < SCENE_COUNT; i++) {
            (void)printf("RENDER scene=%s name=%s current=%d\n", scenes[i]->key, scenes[i]->name,
                         i == current_scene_index);
        }
        (void)printf("RENDER_END\n");
        (void)fflush(stdout);
        return true;
    }
    if (strncmp(args, "scene ", 6) == 0) {
        for (int i = 0; i < SCENE_COUNT; i++) {
            if (strcmp(scenes[i]->key, args + 6) == 0) {
                scene_requested = i;
                (void)printf("RENDER scene=%s\nRENDER_END\n", scenes[i]->key);
                (void)fflush(stdout);
                return true;
            }
        }
        (void)printf("RENDER_ERR unknown scene '%s'; `render scenes` lists them\n", args + 6);
        (void)fflush(stdout);
        return true;
    }
    return false;
}

APP_CONSOLE("render", render_lab_console);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* What a screenshot needs to say two captures measured the same thing. */
static void
render_lab_diagnostic_json(char* out, size_t len) {
    if (snprintf(out, len, "{\"scene\":\"%s\",\"menu\":%s,\"scale\":%d}", current_scene()->key,
                 menu_open ? "true" : "false", render_lab_scale())
        < 0) {
        out[0] = '\0';
    }
}
#endif

app_t app_render_lab = {
    .name = "Render Lab",
    .summary = "Baked mesh flythroughs",
    .enter = render_lab_enter,
    .frame = render_lab_frame,
    .update = render_lab_update,
    .exit = render_lab_exit,
    .home_gesture = true,
#if CONFIG_LAUNCHER_DEVELOPMENT
    .diagnostic_json = render_lab_diagnostic_json,
#endif
    .console = APP_CONSOLE_PTR(render_lab_console),
};

APP_REGISTER(app_render_lab);
