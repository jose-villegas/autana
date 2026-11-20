/* The host harness enters the registered screen with synthetic time and
 * input. Hardware-facing entry code links here because it needs no board
 * access. shell_home_edge returns the bottom edge because synthetic input has
 * no home gesture. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app.h"
#include "app/app_arena.h"
#include "apps/render_lab/render_lab.h"
#include "apps/render_lab/render_lab_view.h"
#include "core/tune.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "input/input.h"
#include "math/text/math_text.h"
#include "render/context/render_context.h"
#include "render/resolution/resolution.h"
#include "render/view/raster_overdraw.h"
#include "render_counts.h"
#include "render_host.h"
#include "scene/scene_internal.h"
#include "scene/scene_shell.h"
#include "scene/transform/transform.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

static int contact_shape = -1;
static int contact_warp = -1;

static int option_count;
static char** option_args;

/* --menu: BOOT on the first frame opens the scene menu over the scene. */
static bool menu;

/* --tap X,Y: one press and release there, in panel coordinates, from TAP_FRAME. */
#define TAP_FRAME 2
static bool tap;
static render_input_step_t touch[] = {
    {TAP_FRAME, true, {0, 0}},
    {TAP_FRAME + 1, false, {0, 0}},
};

/* No IMU on a host: a scene reading motion sees none, as on a board without one. */
bool
input_read_motion_batch(input_motion_batch_t* out) {
    (void)out;
    return false;
}

/* --console FRAME:WORDS, repeatable: the board's `render WORDS` command,
 * such as "clip walk 400", run before frame FRAME draws. */
#define CONSOLE_STEPS_MAX 16

typedef struct {
    int frame;
    const char* words;
} console_step_t;

static console_step_t console_steps[CONSOLE_STEPS_MAX];
static int console_step_count;

/* --size WxH: the scene drawn at that render size, either axis on its own,
 * as one dynamic-resolution step; 0 keeps the scene's own scale. */
static recti_t size_rect;

/* --dump DIR: each frame's raster colour then depth, before the upscale, as
 * DIR/NNNN.raw: width and height as int32, then width x height RGB565, then
 * width x height inverse depth (r3d_span.h), all little-endian. For the frame
 * generation experiment (launcher/tools/r3d/framegen/); host only. */
#define DUMP_PATH_MAX 512
static const char* dump_dir;

/* --counts PATH, with --view cull: a row per frame of what the cull view counted (render_counts.h). */
static const char* counts_path;
static FILE* counts;

/* --transforms PATH: local transforms after each update, keyed by entity name. */
static const char* transforms_path;

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

/* --small-fill NAME: the SMALL FILL menu's preset of that label. */
static bool
small_fill_from_name(const char* name) {
    int count;
    const char* const* names = render_lab_small_fill_names(&count);
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], name) == 0) {
            return render_lab_small_fill_select(i);
        }
    }
    (void)fprintf(stderr, "render_lab_render_host: --small-fill is one of");
    for (int i = 0; i < count; i++) {
        (void)fprintf(stderr, "%s \"%s\"", i == 0 ? "" : ",", names[i]);
    }
    (void)fprintf(stderr, ", not %s\n", name);
    return false;
}

static bool
tune_options(int argc, char** argv, const char* option) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], option) != 0) {
            continue;
        }
        if (++i == argc || !render_host_set_tunable_text(argv[i])) {
            return false;
        }
    }
    return true;
}

static bool
console_options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--console") != 0) {
            continue;
        }
        const char* step = i + 1 < argc ? argv[++i] : "";
        const char* words = strchr(step, ':');
        int frame;
        if (words == NULL || sscanf(step, "%d:", &frame) != 1 || frame < 0 || words[1] == '\0'
            || console_step_count == CONSOLE_STEPS_MAX) {
            (void)fprintf(stderr, "render_lab_render_host: --console is FRAME:WORDS, at most %d of them, not %s\n",
                          CONSOLE_STEPS_MAX, step);
            return false;
        }
        console_steps[console_step_count++] = (console_step_t){frame, words + 1};
    }
    return true;
}

static void
apply_flags(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-hud") == 0) {
            render_lab.show_hud = false;
        } else if (strcmp(argv[i], "--menu") == 0) {
            menu = true;
        }
    }
}

static bool
tap_option(int argc, char** argv) {
    const char* at;
    if (!option_value(argc, argv, "--tap", &at)) {
        return false;
    }
    if (at == NULL) {
        return true;
    }
    if (!vec2i_parse(at, NULL, &touch[0].point)) {
        (void)fprintf(stderr, "render_lab_render_host: --tap is X,Y in panel pixels, not %s\n", at);
        return false;
    }
    touch[1].point = touch[0].point;
    tap = true;
    return true;
}

static bool
size_option(const char* size) {
    if (!recti_parse(size, NULL, &size_rect) || size_rect.x != 0 || size_rect.y != 0 || size_rect.w <= 0
        || size_rect.h <= 0 || size_rect.w > GFX_WIDTH || size_rect.h > GFX_HEIGHT) {
        (void)fprintf(stderr, "render_lab_render_host: --size is WxH within %dx%d, not %s\n", GFX_WIDTH, GFX_HEIGHT,
                      size);
        return false;
    }
    return true;
}

static bool
contact_options(const char* shape, const char* warp) {
    contact_shape = shape == NULL ? -1 : atoi(shape);
    contact_warp = warp == NULL ? -1 : atoi(warp);
    return contact_shape >= -1 && contact_shape <= SCENE_CONTACT_SILHOUETTE && contact_warp >= -1 && contact_warp <= 1;
}

/* Which names are valid is app_render_lab.c's own knowledge (each scene's
 * .key, lab/lab_scene.h) - this only hands the string through. */
static bool
options(int argc, char** argv) {
    const char* scene;
    const char* view;
    const char* scale;
    const char* size;
    const char* small_fill;
    const char* shape;
    const char* warp;
    if (!tap_option(argc, argv)) {
        return false;
    }

    const struct {
        const char* name;
        const char** out;
    } values[] = {
        {"--scene", &scene},         {"--view", &view},          {"--camera", &render_lab.start_camera},
        {"--scale", &scale},         {"--size", &size},          {"--small-fill", &small_fill},
        {"--dump", &dump_dir},       {"--counts", &counts_path}, {"--transforms", &transforms_path},
        {"--contact-shape", &shape}, {"--contact-warp", &warp},
    };

    for (size_t i = 0; i < sizeof values / sizeof values[0]; i++) {
        if (!option_value(argc, argv, values[i].name, values[i].out)) {
            return false;
        }
    }
    if (size != NULL && !size_option(size)) {
        return false;
    }
    if (scene == NULL) {
        (void)fprintf(stderr, "render_lab_render_host needs --scene <key>\n");
        return false;
    }
    if (counts_path != NULL && (view == NULL || strcmp(view, "cull") != 0)) {
        (void)fprintf(stderr, "render_lab_render_host: --counts needs --view cull\n");
        return false;
    }
    if (!contact_options(shape, warp)) {
        return false;
    }
    option_count = argc;
    option_args = argv;
    apply_flags(argc, argv);
    render_lab.start_key = scene;
    return console_options(argc, argv) && (view == NULL || view_from_name(view))
           && (small_fill == NULL || small_fill_from_name(small_fill))
           && (scale == NULL || render_host_set_tunable("render_lab.scale", atoi(scale)))
           && tune_options(argc, argv, "--tune");
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
    ui_init();
    gfx_set_frame_overlay(ui_replay_band);
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    (void)scene_set_take(app_arena_take); /* as shell/scene_system.c does on the board */
    registered->enter();
    if (render_lab.start_camera != NULL && !scene_has_active_camera()) {
        (void)fprintf(stderr, "--camera: no camera %s in this scene\n", render_lab.start_camera);
        return false;
    }
    if (size_rect.w > 0) {
        const resolution_step_t size = {size_rect.w, size_rect.h};
        const resolution_config_t one = resolution_config(&size, 1, 1, INT32_MAX);
        render_context_set_dynamic_resolution(render_context_main(), &one, NULL, 0);
    }
    /* Last, so that no failure after it leaves the file open. */
    return tune_options(option_count, option_args, "--live-tune")
           && (counts_path == NULL || (counts = render_counts_open(counts_path, "render_lab_render_host")) != NULL);
}

/* With --view overdraw, the last frame's totals on stderr, beside its image. */
static void
report_overdraw(const provider_frame_t* frame) {
    const render_context_t* context = render_context_main();
    if (frame->index + 1 != frame->count || context->debug_view != render_context_debug_view_named("overdraw")
        || context->debug_view_state == NULL) {
        return;
    }
    const raster_overdraw_frame_t f = raster_overdraw_frame(context->debug_view_state);
    (void)fprintf(stderr, RASTER_OVERDRAW_LINE "\n", f.pixels, f.tested, f.written, f.tested_max, f.written_max);
}

static void
dump_raster(int index) {
    const raster_t* raster = &render_context_main()->raster;
    char path[DUMP_PATH_MAX];
    (void)snprintf(path, sizeof path, "%s/%04d.raw", dump_dir, index);
    /* The raster's own size: the one its colour and depth rows are laid out at. */
    const int32_t size[2] = {raster->width, raster->height};
    const size_t pixels = (size_t)raster->width * (size_t)raster->height;
    FILE* out = fopen(path, "wb");
    const bool written = out != NULL && fwrite(size, sizeof size[0], 2, out) == 2
                         && fwrite(raster_color(raster), sizeof(gfx_color_t), pixels, out) == pixels
                         && fwrite(raster_depth(raster), sizeof(uint16_t), pixels, out) == pixels;
    if (out == NULL || fclose(out) != 0 || !written) {
        (void)fprintf(stderr, "render_lab_render_host: cannot write %s\n", path);
        exit(EXIT_FAILURE);
    }
}

static void
apply_contact_options(void) {
    const lab_scene_t* shown = render_lab.scenes[render_lab.state.scene];
    if (shown->path == NULL || (contact_shape < 0 && contact_warp < 0)) {
        return;
    }
    const lab_path_t path = shown->path();
    const entity_t entity = path.entity;
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    const int index = scene == NULL ? -1 : scene_renderer_index(scene, row);
    if (index < 0) {
        return;
    }
    const scene_renderer_t* r = &scene->renderers[index];
    const scene_contact_t contact = {r->contact_blob_ground_y, r->contact_blob_strength,
                                     contact_shape >= 0 ? (scene_contact_shape_t)contact_shape : r->contact_blob_shape,
                                     contact_warp >= 0 ? contact_warp != 0 : r->contact_warp};
    scene_set_contact(entity, &contact);
}

static void
dump_transforms(int frame) {
    FILE* out = fopen(transforms_path, frame == 0 ? "w" : "a");
    if (out == NULL) {
        perror(transforms_path);
        exit(EXIT_FAILURE);
    }
    if (frame == 0) {
        (void)fprintf(out, "frame,name,x,y,z,qx,qy,qz,qw\n");
    }
    const lab_scene_t* current = lab_current(&render_lab);
    const lab_path_t path = current != NULL && current->path != NULL
                                ? current->path()
                                : (lab_path_t){.scene = ENTITY_NONE, .entity = ENTITY_NONE};
    for (entity_t e = entity_next(path.scene, ENTITY_NONE); !handle_is_none(e); e = entity_next(path.scene, e)) {
        const transformf_t local = transform_local(e);
        (void)fprintf(out, "%d,%s,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", frame, entity_name(e),
                      (double)local.position.x, (double)local.position.y, (double)local.position.z,
                      (double)local.rotation.x, (double)local.rotation.y, (double)local.rotation.z,
                      (double)local.rotation.w);
    }
    if (fclose(out) != 0) {
        perror(transforms_path);
        exit(EXIT_FAILURE);
    }
}

static void
draw(const provider_frame_t* frame) {
    ui_clear_band_overlay();
    input_t in = tap ? frame->input : (input_t){0};
    in.boot.pressed = menu && frame->index == 0;
    for (int i = 0; i < console_step_count; i++) {
        if (console_steps[i].frame == frame->index) {
            (void)render_lab_console(console_steps[i].words);
        }
    }
    if (frame->index == 0) {
        apply_contact_options();
    }
    app_list()->frame(frame->dt_ms, &in);
    report_overdraw(frame);
    if (counts != NULL) {
        render_counts_row(&counts, frame);
    }
    if (dump_dir != NULL) {
        dump_raster(frame->index);
    }
    /* The shell's update() runs before it composes a frame; this harness has
     * already composed, so it steers the next one. */
    app_list()->update(frame->dt_ms, &in);
    if (transforms_path != NULL) {
        dump_transforms(frame->index);
    }
}

const frame_provider_t render_lab_provider = {
    .name = "render_lab",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .input = touch,
    .input_count = sizeof touch / sizeof touch[0],
    .options = options,
    .setup = setup,
    .draw = draw,
};
