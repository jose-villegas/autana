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
#include <string.h>

#include "app.h"
#include "apps/render_lab/render_lab_view.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

/* The band ring keeps no retained frame for render_host.c to read back, so
 * setup() asks for the full-framebuffer layout. */
extern bool render_lab_band_mode;
extern bool render_lab_show_hud;
extern const char* render_lab_start_scene_key;

static int shell_quarter;

/* The value after `name`, or NULL when it is absent. */
static const char*
option_value(int argc, char** argv, const char* name) {
    for (int i = 0; i + 1 < argc; i++) {
        if (strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return NULL;
}

static bool
view_from_name(const char* name) {
    static const struct {
        const char* name;
        r3d_lit_view_mode_t mode;
    } views[] = {
        {"shaded", R3D_LIT_VIEW_SHADED},
        {"depth", R3D_LIT_VIEW_DEPTH},
        {"tiles", R3D_LIT_VIEW_DEPTH_TILES},
    };

    for (size_t i = 0; i < sizeof views / sizeof views[0]; i++) {
        if (strcmp(name, views[i].name) == 0) {
            render_lab_view_mode = views[i].mode;
            return true;
        }
    }
    fprintf(stderr, "render_lab_render_host: --view is shaded, depth or tiles, not %s\n", name);
    return false;
}

/* Which names are valid is app_render_lab.c's own knowledge (each scene's
 * .key, render_lab_scene.h) - this only hands the string through. */
static bool
options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-hud") == 0) {
            render_lab_show_hud = false;
        }
    }
    render_lab_start_scene_key = option_value(argc, argv, "--scene");
    if (render_lab_start_scene_key == NULL) {
        fprintf(stderr, "render_lab_render_host needs --scene <key>\n");
        return false;
    }
    const char* view = option_value(argc, argv, "--view");
    return view == NULL || view_from_name(view);
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
    if (render_lab_view_mode != R3D_LIT_VIEW_SHADED && !render_lab_scene_shows_views()) {
        fprintf(stderr, "--view: the scene %s has no depth to show\n", render_lab_start_scene_key);
        return false;
    }
    return true;
}

static void
draw(const render_frame_t* frame) {
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
