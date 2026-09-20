/*
 * wire_render_host - this app itself, entered and stepped on a host, started
 * on a chosen wire scene via render_lab_start_scene_index.
 *
 * A render_host.h scene, the same shape cube_render_host.c uses: the real
 * enter()/frame(), the real gfx and ui layers, no board, no IDF header, no
 * clock or finger but the harness's own.
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
 * this asks for the full-framebuffer layout, the same reason
 * cube_render_host.c does. render_lab_start_scene_index picks the scene;
 * see app_render_lab.c's scenes[] table for what each index means. */
extern bool render_lab_band_mode;
extern int render_lab_start_scene_index;

#define SCENE_WIRE_PLANE   1
#define SCENE_WIRE_CUBE    2
#define SCENE_WIRE_SPHERE  3
#define SCENE_WIRE_CAPSULE 4

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
options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--scene") != 0 || i + 1 >= argc) {
            continue;
        }
        const char* name = argv[i + 1];
        if (strcmp(name, "plane") == 0) {
            render_lab_start_scene_index = SCENE_WIRE_PLANE;
        } else if (strcmp(name, "cube") == 0) {
            render_lab_start_scene_index = SCENE_WIRE_CUBE;
        } else if (strcmp(name, "sphere") == 0) {
            render_lab_start_scene_index = SCENE_WIRE_SPHERE;
        } else if (strcmp(name, "capsule") == 0) {
            render_lab_start_scene_index = SCENE_WIRE_CAPSULE;
        } else {
            fprintf(stderr, "unknown --scene %s\n", name);
            return false;
        }
        return true;
    }
    fprintf(stderr, "wire_render_host needs --scene plane|cube|sphere|capsule\n");
    return false;
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
    .name = "wire",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
};
