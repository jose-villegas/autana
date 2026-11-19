#!/bin/sh
# Firmware launcher sources and host setup shared by home and boot scenes.
scene_ui=1
launcher_home_sources="
main/render/render_view.c
main/app/app_registry.c
main/shell/ui/ui_launcher.c
main/shell/ui/ui_launcher_draw.c
main/shell/ui/ui_ridge.c
tools/render/scenes/launcher_home_render_setup.c
"
launcher_home_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0"
# The engine pack holds the launcher tiles' skin; a scene that draws the
# launcher builds it in, or its tiles fall back to plain boxes.
launcher_home_assets="main/engine"
