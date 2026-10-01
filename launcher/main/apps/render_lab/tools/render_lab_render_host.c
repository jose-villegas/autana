/*
 * render_lab_render_host - this app itself, entered and stepped on a host,
 * started on the scene --scene names.
 *
 * A render_host.h scene: the real enter()/frame(), the real gfx and ui
 * layers, no board, no IDF header, no clock or finger but the harness's own.
 * app_*.c is excluded from the host test runner as hardware-facing, but it
 * asks nothing of the board, so this links the real app_registry.c - its
 * own APP_REGISTER() constructor lands the one app that runs here - and
 * only display_shell_quarter() below stands in for the shell itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "apps/render_lab/render_lab_view.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "scene/scene.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"
#include "util/tune.h"

/* The band ring keeps no retained frame for render_host.c to read back, so
 * setup() asks for the full-framebuffer layout. */
extern bool render_lab_band_mode;
extern bool render_lab_show_hud;
extern const char* render_lab_start_scene_key;

static int shell_quarter;

/* The value after `name` in `out`, NULL when the option is absent; false,
 * with a message, when it is the last argument and has none. */
static bool
option_value(int argc, char** argv, const char* name, const char** out) {
    *out = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], name) != 0) {
            continue;
        }
        if (i + 1 >= argc) {
            (void)fprintf(stderr, "render_lab_render_host: %s needs a value\n", name);
            return false;
        }
        *out = argv[i + 1];
    }
    return true;
}

static bool tune_set;

static void
note_reply(const char* line) {
    tune_set = strncmp(line, "TUNE_OK", 7) == 0;
    if (!tune_set) {
        (void)fprintf(stderr, "render_lab_render_host: %s\n", line);
    }
}

/* Sets one render_lab tunable, the way the console would. */
static bool
set_tunable(const char* name, int value) {
    char line[TUNE_NAME_MAX + 16];
    if (snprintf(line, sizeof line, "SET render_lab.%s %d", name, value) < 0) {
        return false;
    }
    tune_set = false;
    (void)tune_handle_line(line, note_reply);
    return tune_set;
}

static bool
view_from_name(const char* name) {
    static const char* const views[] = {"shaded", "depth", "tiles"};
    for (int i = 0; i < (int)(sizeof views / sizeof views[0]); i++) {
        if (strcmp(name, views[i]) == 0) {
            return set_tunable("view", i);
        }
    }
    (void)fprintf(stderr, "render_lab_render_host: --view is shaded, depth or tiles, not %s\n", name);
    return false;
}

static void
apply_flags(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-hud") == 0) {
            render_lab_show_hud = false;
        }
    }
}

/* Which names are valid is app_render_lab.c's own knowledge (each scene's
 * .key, render_lab_scene.h) - this only hands the string through. */
static bool
options(int argc, char** argv) {
    const char* scene;
    const char* view;
    const char* scale;
    if (!option_value(argc, argv, "--scene", &scene) || !option_value(argc, argv, "--view", &view)
        || !option_value(argc, argv, "--scale", &scale)) {
        return false;
    }
    if (scene == NULL) {
        (void)fprintf(stderr, "render_lab_render_host needs --scene <key>\n");
        return false;
    }
    apply_flags(argc, argv);
    render_lab_start_scene_key = scene;
    return (view == NULL || view_from_name(view)) && (scale == NULL || set_tunable("scale", atoi(scale)));
}

int
display_shell_quarter(void) {
    return shell_quarter;
}

static bool
setup(int quarter) {
    const app_t* registered = app_list();
    if (registered == NULL || registered->enter == NULL || registered->frame == NULL) {
        fprintf(stderr, "no app registered itself\n");
        return false;
    }
    render_lab_band_mode = false;
    shell_quarter = quarter;
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    registered->enter();
    if (render_lab_view() != RASTER_SHOW_SHADED && !render_lab_scene_shows_views()) {
        (void)fprintf(stderr, "--view: the scene %s has no depth to show\n", render_lab_start_scene_key);
        return false;
    }
    return true;
}

static void
draw(const render_frame_t* frame) {
    if (scene_has_active_camera()) {
        scene_shell_compose(frame->dt_ms); /* what the shell does before frame() */
    }
    app_list()->frame(frame->dt_ms, &frame->input);
}

const render_scene_t render_scene = {
    .name = "render_lab",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
};
