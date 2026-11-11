/* app_render_lab: mesh scenes behind the lab's HUD and scene picker (lab/lab.h). */
#include <stdio.h>

#include "app/app.h"
#include "apps/render_lab/ui/render_lab_settings.h"
#include "console/render_lab_fly.h"
#include "render/context/render_context.h"
#include "render_lab.h"
#include "render_lab_dynres.h"
#include "render_lab_view.h"
#include "scene_baked.h"
#include "stress/scene_stress.h"

/* Each stress preset, in the order its table lists them. */
#define STRESS_SCENE_ENTRY(id_, ...) &scene_stress[STRESS_##id_],

/* Index 0 is the scene a start with no key shows; a family's scenes keep this order in its dropdown. */
static const lab_scene_t* const scenes[] = {&scene_sponza[SCENE_BAKED_FITTED_FULL],
                                            &scene_sponza[SCENE_BAKED_FITTED],
                                            &scene_sponza[SCENE_BAKED_FULL],
                                            &scene_sponza[SCENE_BAKED_LITE],
                                            &scene_sponza[SCENE_BAKED_FLAT],
                                            &scene_sponza[SCENE_BAKED_FLAT_FITTED],
                                            &scene_capybara,
                                            &scene_water,
                                            &scene_bistro[SCENE_BAKED_FITTED_FULL],
                                            &scene_bistro[SCENE_BAKED_FITTED],
                                            &scene_bistro[SCENE_BAKED_FULL],
                                            &scene_bistro[SCENE_BAKED_LITE],
                                            &scene_bistro[SCENE_BAKED_FLAT],
                                            &scene_bistro[SCENE_BAKED_FLAT_FITTED],
                                            STRESS_PRESETS(STRESS_SCENE_ENTRY)};
#define SCENE_COUNT ((int)(sizeof(scenes) / sizeof(scenes[0])))

static void
leave_scene(void) {
    render_lab_dynres_stop();
    render_lab_fly_stop();
}

/* Frame generation for a scene where nothing moves on its own, when render_lab.framegen asks. */
static void
frame_begin(const lab_scene_t* scene) {
    render_context_set_frame_generation(render_context_main(), scene->generates_frames && render_lab_framegen_on());
}

lab_t render_lab = {
    .app = &app_render_lab,
    .console_word = "render",
    .reply_tag = "RENDER",
    .scenes = scenes,
    .scene_count = SCENE_COUNT,
    .hooks =
        {
            .scene_settle = render_lab_dynres_apply,
            .scene_leaving = leave_scene,
            .frame_begin = frame_begin,
            .update_end = render_lab_fly_frame,
            .console = render_lab_fly_console,
            .layout = render_lab_layout,
            .test_render = render_lab_apply_raster_settings,
            .settings = render_lab_settings_draw,
        },
    .show_hud = true,
};

static void
render_lab_enter(void) {
    lab_enter(&render_lab);
}

static void
render_lab_frame(uint32_t dt_ms, const input_t* input) {
    lab_frame(&render_lab, dt_ms, input);
}

static void
render_lab_update(uint32_t dt_ms, const input_t* input) {
    lab_update(&render_lab, dt_ms, input);
}

static void
render_lab_exit(void) {
    lab_exit(&render_lab);
}

/* "render scenes", "render scene <key>" and "render pose <ms> | play" are the
 * lab's (lab_console()); "render fly" flies the scene (render_lab_fly.h).
 * Other words go to the showing scene's console hook, if it has one. */
bool
render_lab_console(const char* args) {
    return lab_console(&render_lab, args);
}

APP_CONSOLE("render", render_lab_console);

#if CONFIG_LAUNCHER_DEVELOPMENT
/* What a screenshot needs to say two captures measured the same thing. */
static void
render_lab_diagnostic_json(char* out, size_t len) {
    if (snprintf(out, len, "{\"scene\":\"%s\",\"menu\":%s,\"scale\":%d}", lab_current(&render_lab)->key,
                 render_lab.state.menu_open ? "true" : "false", render_lab_scale())
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
