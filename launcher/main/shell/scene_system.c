/*
 * The scene manager as a shell system: drawn while the last frame is sent,
 * upscaled into the framebuffer before the app's frame(), and unloaded when
 * the app that loaded it exits. Its scenes live in the app arena, lent for
 * the visit like the rest of the app's memory. Registered here because
 * scene/ sits below the shell and cannot see shell_system.h or app/.
 */
#include "app/app_arena.h"
#include "scene/scene_shell.h"
#include "shell/shell_system.h"

static shell_system_t scene_system = {
    .name = "scene",
    .order = SHELL_ORDER_SCENE,
    .update = scene_shell_render,
    .compose = scene_shell_compose,
    .app_exit = scene_unload_all,
    .overlaps_present = scene_has_active_camera,
};

SHELL_SYSTEM_REGISTER(scene_system);

__attribute__((constructor)) static void
scene_system_lend_arena(void) {
    scene_set_take(app_arena_take);
}
