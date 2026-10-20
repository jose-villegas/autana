/* app_render_lab: mesh scenes behind a shared HUD and scene picker. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "apps/render_lab/ui/render_lab_hud_screen.h"
#include "apps/render_lab/ui/render_lab_menu_screen.h"
#include "build/build_variant.h"
#include "draw/draw.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "render_lab.h"
#include "render_lab_scene.h"
#include "render_lab_view.h"
#include "scene/scene.h"
#include "services/tune.h"
#include "ui/ui.h"

extern const render_lab_scene_t scene_sponza;
extern const render_lab_scene_t scene_capybara;
extern const render_lab_scene_t scene_sponza_lite;
extern const render_lab_scene_t scene_sponza_flat;
extern const render_lab_scene_t scene_sponza_fitted;
extern const render_lab_scene_t scene_sponza_fitted_full;
extern const render_lab_scene_t scene_sponza_flat_fitted;

/* Index 0 is the scene a start with no key shows; a family's scenes keep this order in its dropdown. */
static const render_lab_scene_t* const scenes[] = {
    &scene_sponza_fitted_full, &scene_sponza_fitted,      &scene_sponza,   &scene_sponza_lite,
    &scene_sponza_flat,        &scene_sponza_flat_fitted, &scene_capybara,
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
    scene_requested = -1;
    last_layout_generation = ui_layout_generation();
}

static void
switch_to_scene(int index) {
    current_scene()->exit();
    current_scene_index = index;
    render_lab_start_scene_key = current_scene()->key; /* keeps a later re-entry on this same scene */
    /* The new scene clears the full framebuffer, so discard the composed half picture first. */
    gfx_mode_exit();
    enter_layout();
    ui_invalidate();
    current_scene()->enter();
    scene_title_remaining_ms = SCENE_TITLE_MS;
}

/* Scene families in the order their first scene appears. */
static int
family_names(const char* names[SCENE_COUNT]) {
    int count = 0;
    for (int i = 0; i < SCENE_COUNT; i++) {
        int seen = 0;
        while (seen < count && strcmp(names[seen], scenes[i]->family) != 0) {
            seen++;
        }
        if (seen == count) {
            names[count++] = scenes[i]->family;
        }
    }
    return count;
}

int
render_lab_family_variants(const char* family, const char* const** names, int* selected) {
    static const char* variants[SCENE_COUNT];
    int count = 0;
    *selected = -1;
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (strcmp(scenes[i]->family, family) == 0) {
            if (i == current_scene_index) {
                *selected = count;
            }
            variants[count++] = scenes[i]->variant;
        }
    }
    *names = variants;
    return count;
}

bool
render_lab_request_variant(const char* family, int index) {
    int count = 0;
    for (int i = 0; i < SCENE_COUNT; i++) {
        if (strcmp(scenes[i]->family, family) == 0 && count++ == index) {
            scene_requested = i;
            return true;
        }
    }
    return false;
}

static void
draw_fps(const input_t* input) {
    ui_begin(input);

    const render_lab_hud_screen_state_t state = {
        .fps_value = fps_value,
        .scene_title = current_scene()->name,
        .scene_title_alpha = scene_title_alpha(),
        .status = current_scene()->status != NULL ? current_scene()->status() : NULL,
    };
    render_lab_hud_screen_draw(&state);

    /* The scene changes underneath the HUD every frame, so the overlay
     * must composite over the current scene without an opaque window. */
    ui_end(UI_NO_BACKGROUND);
}

static void
draw_menu(const input_t* input) {
    ui_begin(input);

    const char* families[SCENE_COUNT];
    const int family_count = family_names(families);
    int family = 0;
    while (strcmp(families[family], current_scene()->family) != 0) {
        family++;
    }
    const render_lab_menu_screen_state_t state = {
        .families = families,
        .family_count = family_count,
        .family = family,
        .controls = current_scene()->controls,
    };
    const render_lab_menu_screen_result_t result = render_lab_menu_screen_draw(&state);

    if (result.family_picked >= 0 && result.family_picked != family) {
        (void)render_lab_request_variant(families[result.family_picked], 0);
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
        draw_menu(input);
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

/* "render scenes" lists each scene's key and name and which one shows; "render scene <key>" switches to that exact key at the next frame. Replies are "RENDER ..." lines then "RENDER_END", or "RENDER_ERR <why>". Other words go to the showing scene's console hook, if it has one. */
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
    return current_scene()->console != NULL && current_scene()->console(args, console_reply_stdio);
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
