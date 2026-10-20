/* The host harness enters the registered screen with synthetic time and
 * input. Hardware-facing entry code links here because it needs no board
 * access. display_quarter_now returns the requested orientation; shell_home_edge
 * returns the bottom edge because synthetic input has no home gesture. */

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
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

static int shell_quarter;
static int option_count;
static char** option_args;

/* --menu: BOOT on the first frame opens the scene menu over the scene. */
static bool menu;

/* --tap X,Y: one press and release there, in panel coordinates, from TAP_FRAME. */
#define TAP_FRAME 2
static bool tap;
static render_input_step_t touch[] = {
    {TAP_FRAME, true, 0, 0},
    {TAP_FRAME + 1, false, 0, 0},
};

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

static bool tune_succeeded;

static void
note_reply(const char* line) {
    tune_succeeded = strncmp(line, "TUNE_OK ", sizeof "TUNE_OK " - 1) == 0;
    if (!tune_succeeded) {
        (void)fprintf(stderr, "render_lab_render_host: %s\n", line);
    }
}

static bool
view_from_name(const char* name) {
    const int view = render_context_debug_view_named(name);
    if (view != RENDER_DEBUG_VIEW_UNKNOWN) {
        return render_host_set_tunable("render_lab.view", view);
    }
    (void)fprintf(stderr, "render_lab_render_host: --view is ");
    render_context_print_debug_views(stderr);
    (void)fprintf(stderr, ", not %s\n", name);
    return false;
}

static bool
tune_options(int argc, char** argv, const char* option) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], option) != 0) {
            continue;
        }
        if (++i == argc) {
            return false;
        }
        if (strchr(argv[i], '=') == NULL) {
            return false;
        }
        const size_t length = strlen(argv[i]);
        char* line = malloc(sizeof "SET " + length);
        if (line == NULL) {
            return false;
        }
        memcpy(line, "SET ", sizeof "SET " - 1);
        strcpy(line + sizeof "SET " - 1, argv[i]);
        char* equal = strchr(line, '=');
        *equal = ' ';
        tune_succeeded = false;
        (void)tune_handle_line(line, note_reply);
        free(line);
        if (!tune_succeeded) {
            return false;
        }
    }
    return true;
}

static void
apply_flags(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-hud") == 0) {
            render_lab_show_hud = false;
        } else if (strcmp(argv[i], "--menu") == 0) {
            menu = true;
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
    const char* at;
    if (!option_value(argc, argv, "--tap", &at)) {
        return false;
    }
    if (at != NULL) {
        if (sscanf(at, "%d,%d", &touch[0].x, &touch[0].y) != 2) {
            (void)fprintf(stderr, "render_lab_render_host: --tap is X,Y in panel pixels, not %s\n", at);
            return false;
        }
        touch[1].x = touch[0].x;
        touch[1].y = touch[0].y;
        tap = true;
    }
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
    option_count = argc;
    option_args = argv;
    apply_flags(argc, argv);
    render_lab_start_scene_key = scene;
    return (view == NULL || view_from_name(view))
           && (scale == NULL || render_host_set_tunable("render_lab.scale", atoi(scale)))
           && tune_options(argc, argv, "--tune");
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
        (void)fprintf(stderr, "no app registered itself\n");
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
    return tune_options(option_count, option_args, "--live-tune");
}

static void
draw(const render_frame_t* frame) {
    ui_clear_band_overlay();
    input_t in = tap ? frame->input : (input_t){0};
    in.boot.pressed = menu && frame->index == 0;
    app_list()->frame(frame->dt_ms, &in);
}

const render_scene_t render_scene = {
    .name = "render_lab",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .input = touch,
    .input_count = sizeof touch / sizeof touch[0],
    .options = options,
    .setup = setup,
    .draw = draw,
    .draw_band = scene_shell_compose_band,
    .band_overlay = ui_replay_band,
};
