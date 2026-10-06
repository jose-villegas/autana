/*
 * The scene manager as a shell system: drawn while the last frame is sent,
 * upscaled into the framebuffer before the app's frame(), and unloaded when
 * the app that loaded it exits. Registered here because scene/ sits below
 * the shell and cannot see shell_system.h.
 */
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
