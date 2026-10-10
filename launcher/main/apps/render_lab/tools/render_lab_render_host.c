/*
 * render_lab_render_host - this app itself, entered and stepped on a host,
 * started on the scene --scene names.
 *
 * A render_host.h scene: the real enter()/frame(), the real gfx and ui
 * layers, no board, no IDF header, no clock or finger but the harness's own.
 * app_*.c is excluded from the host test runner as hardware-facing, but it
 * asks nothing of the board, so this links the real app_registry.c - its
 * own APP_REGISTER() constructor lands the one app that runs here - and
 * only display_quarter_now() and shell_home_edge() below stand in for the
 * shell itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "render/context/render_context.h"
#include "render/resolution/resolution.h"
#include "render_host.h"
#include "scene/scene_shell.h"
#include "services/tune.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

static int shell_quarter;

/* --size WxH: the scene drawn at that render size, either axis on its own,
 * as one dynamic-resolution step; 0 keeps the scene's own scale. */
static int size_width, size_height;

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
    const int view = render_context_debug_view_named(name);
    if (view != RENDER_DEBUG_VIEW_UNKNOWN) {
        return set_tunable("view", view);
    }
    (void)fprintf(stderr, "render_lab_render_host: --view is ");
    render_context_print_debug_views(stderr);
    (void)fprintf(stderr, ", not %s\n", name);
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
    const char* size;
    if (!option_value(argc, argv, "--scene", &scene) || !option_value(argc, argv, "--view", &view)
        || !option_value(argc, argv, "--camera", &render_lab_start_camera)
        || !option_value(argc, argv, "--scale", &scale) || !option_value(argc, argv, "--size", &size)) {
        return false;
    }
    if (size != NULL
        && (sscanf(size, "%dx%d", &size_width, &size_height) != 2 || size_width <= 0 || size_height <= 0
            || size_width > GFX_WIDTH || size_height > GFX_HEIGHT)) {
        (void)fprintf(stderr, "render_lab_render_host: --size is WxH within %dx%d, not %s\n", GFX_WIDTH, GFX_HEIGHT,
                      size);
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
display_quarter_now(void) {
    return shell_quarter;
}

/* No finger touches a host render, so no touch ever lands near this edge. */
gesture_edge_t
shell_home_edge(void) {
    return GESTURE_EDGE_BOTTOM;
}

static bool
setup(int quarter) {
    const app_t* registered = app_list();
    if (registered == NULL || registered->enter == NULL || registered->frame == NULL) {
        fprintf(stderr, "no app registered itself\n");
        return false;
    }
    shell_quarter = quarter;
    ui_init();
    gfx_set_frame_overlay(ui_replay_band);
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    registered->enter();
    if (render_lab_start_camera != NULL && !scene_has_active_camera()) {
        (void)fprintf(stderr, "--camera: no camera %s in this scene\n", render_lab_start_camera);
        return false;
    }
    if (size_width > 0) {
        const resolution_step_t size = {size_width, size_height};
        const resolution_config_t one = resolution_config(&size, 1, 1, INT32_MAX);
        render_context_set_dynamic_resolution(render_context_main(), &one, NULL, 0);
    }
    return true;
}

static void
draw(const render_frame_t* frame) {
    ui_clear_band_overlay();
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
