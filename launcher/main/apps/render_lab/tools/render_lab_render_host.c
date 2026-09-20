/*
 * render_lab_render_host - this app itself, entered and stepped on a host,
 * started on the scene --scene names.
 *
 * A render_host.h scene: the real enter()/frame(), the real gfx and ui
 * layers, no board, no IDF header, no clock or finger but the harness's own.
 * app_*.c is excluded from the host test runner as hardware-facing, but it
 * asks nothing of the board, so the shell-owned functions below stand in - a
 * registry of exactly the app that registered itself, the shell's quarter,
 * and no frame loop at all.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"

/* The band ring keeps no retained frame for render_host.c to read back, so
 * setup() asks for the full-framebuffer layout. */
extern bool render_lab_band_mode;
extern bool render_lab_show_hud;
extern int render_lab_start_scene_index;

#define SCENE_GOURAUD      0
#define SCENE_WIRE_PLANE   1
#define SCENE_WIRE_CUBE    2
#define SCENE_WIRE_SPHERE  3
#define SCENE_WIRE_CAPSULE 4
#define SCENE_CORNELL      5

static const app_t* registered;
static int shell_quarter;

void
app_register(const app_t* app) {
    registered = app;
}

const app_t* const*
app_list(void) {
    return &registered;
}

int
app_list_count(void) {
    return registered != NULL ? 1 : 0;
}

static bool
scene_named(const char* name) {
    if (strcmp(name, "gouraud") == 0) {
        render_lab_start_scene_index = SCENE_GOURAUD;
    } else if (strcmp(name, "plane") == 0) {
        render_lab_start_scene_index = SCENE_WIRE_PLANE;
    } else if (strcmp(name, "cube") == 0) {
        render_lab_start_scene_index = SCENE_WIRE_CUBE;
    } else if (strcmp(name, "sphere") == 0) {
        render_lab_start_scene_index = SCENE_WIRE_SPHERE;
    } else if (strcmp(name, "capsule") == 0) {
        render_lab_start_scene_index = SCENE_WIRE_CAPSULE;
    } else if (strcmp(name, "cornell") == 0) {
        render_lab_start_scene_index = SCENE_CORNELL;
    } else {
        fprintf(stderr, "unknown --scene %s\n", name);
        return false;
    }
    return true;
}

static bool
options(int argc, char** argv) {
    bool have_scene = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-hud") == 0) {
            render_lab_show_hud = false;
        } else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            if (!scene_named(argv[i + 1])) {
                return false;
            }
            have_scene = true;
            i++;
        }
    }
    if (!have_scene) {
        fprintf(stderr, "render_lab_render_host needs --scene gouraud|plane|cube|sphere|capsule|cornell\n");
    }
    return have_scene;
}

int
display_shell_quarter(void) {
    return shell_quarter;
}

static bool
setup(int quarter) {
    if (registered == NULL || registered->enter == NULL || registered->frame == NULL) {
        fprintf(stderr, "no app registered itself\n");
        return false;
    }
    render_lab_band_mode = false;
    shell_quarter = quarter;
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    registered->enter();
    return true;
}

static void
draw(const render_frame_t* frame) {
    registered->frame(frame->dt_ms, &frame->input);
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
