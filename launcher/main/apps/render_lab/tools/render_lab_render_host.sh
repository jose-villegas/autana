#!/bin/sh
#
# Render this app's scenes on a host, with no board and no flash cycle.
#
#   ./launcher/main/apps/render_lab/tools/render_lab_render_host.sh [-o <dir>]
#
# Each declared render starts on one scene (--scene) via
# render_lab_start_scene_key and steps 16 ms frames, full-framebuffer
# layout. Landscape is the shipping orientation, so it leads; a panel-native
# render is the shape tools/render/render_diff.sh compares a device capture against.
#
# Everything beyond the declarations below - finding a compiler, building,
# checking each image against its declared size and pin, converting to PNG -
# is launcher/tools/render/render_scene.sh.

set -eu
SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

scene_name=render_lab

scene_ui=1
scene_target=render_host
scene_sources="
main/app/app_arena.c
main/console/console_words.c
main/app/app_registry.c
main/input/touch/gesture.c
main/input/mapping/orbit_motion_touch.c
main/input/motion/gravity.c
main/apps/render_lab/tools/render_lab_render_host.c
$(CDPATH= cd -- "$SCRIPT_DIR/../../../../" &&
    find main/apps/render_lab \( -type d \( -name tools -o -name tests \) -prune \) -o \
        \( -type f -name '*.c' ! -name 'suite_*.c' -print \) | LC_ALL=C sort)
"
# The app region is sized from the board's PSRAM, as on the device.
# shellcheck source=../../../../tools/device/device_profile.sh
. "$SCRIPT_DIR/../../../../tools/device/device_profile.sh"
device_profile_load "" "$SCRIPT_DIR/../../../../tools/device/device_profiles" || exit 1
scene_defines="-DCONFIG_LAUNCHER_DEVELOPMENT=0 -DAPP_ARENA_HOST_PSRAM_BYTES=$(device_profile_require DP_PSRAM_BYTES)"

# The lit-mesh scene's views (--view shaded|depth|tiles|motion|meshlets|cull|overdraw) are raster_show()
# over the same frame, so each is unpinned for the reason the shaded one is: its
# camera path is float, so which pixels a triangle reaches is not identical on
# every compiler. The tests in tools/tests compare the views with each other.
#
scene_renders="
sponza-landscape-hud|--quarter 1 --scene sponza --frames 2|448x368|nopin
sponza-portrait-hud|--quarter 0 --scene sponza --frames 2|368x448|nopin
sponza-title-fading|--quarter 1 --scene sponza --frames 110|448x368|nopin
sponza-landscape|--quarter 1 --no-hud --scene sponza --frames 2|448x368|nopin
sponza-portrait|--quarter 0 --no-hud --scene sponza --frames 2|368x448|nopin
sponza-flipped|--quarter 3 --no-hud --scene sponza --frames 2|448x368|nopin
sponza-flat-landscape|--quarter 1 --no-hud --scene sponza-flat --frames 2|448x368|nopin
sponza-flat-portrait|--quarter 0 --no-hud --scene sponza-flat --frames 2|368x448|nopin
sponza-fitted-landscape|--quarter 1 --no-hud --scene sponza-fitted --frames 2|448x368|nopin
sponza-fitted-portrait|--quarter 0 --no-hud --scene sponza-fitted --frames 2|368x448|nopin
sponza-fitted-full-landscape|--quarter 1 --no-hud --scene sponza-fitted-full --frames 2|448x368|nopin
sponza-fitted-full-portrait|--quarter 0 --no-hud --scene sponza-fitted-full --frames 2|368x448|nopin
sponza-depth-landscape|--quarter 1 --no-hud --scene sponza --frames 2 --view depth|448x368|nopin
sponza-depth-portrait|--quarter 0 --no-hud --scene sponza --frames 2 --view depth|368x448|nopin
sponza-depth-flipped|--quarter 3 --no-hud --scene sponza --frames 2 --view depth|448x368|nopin
sponza-tiles-landscape|--quarter 1 --no-hud --scene sponza --frames 2 --view tiles|448x368|nopin
sponza-tiles-portrait|--quarter 0 --no-hud --scene sponza --frames 2 --view tiles|368x448|nopin
sponza-tiles-flipped|--quarter 3 --no-hud --scene sponza --frames 2 --view tiles|448x368|nopin
menu-sponza-landscape|--quarter 1 --menu --scene sponza --frames 4|448x368
menu-sponza-portrait|--quarter 0 --menu --scene sponza --frames 4|368x448
menu-capybara-landscape|--quarter 1 --menu --scene capybara --frames 4|448x368
menu-capybara-portrait|--quarter 0 --menu --scene capybara --frames 4|368x448
menu-capybara-clips|--quarter 0 --menu --scene capybara --tap 184,172 --frames 6|368x448
menu-sponza-small-fill|--quarter 0 --menu --scene sponza --tap 184,342 --frames 6|368x448
menu-sponza-small-fill-landscape|--quarter 1 --menu --scene sponza --tap 46,224 --frames 6|448x368
menu-sponza-scene|--quarter 0 --menu --scene sponza --tap 184,72 --frames 6|368x448
menu-sponza-mesh|--quarter 0 --menu --scene sponza --tap 184,172 --frames 6|368x448
water-tour|--quarter 0 --no-hud --scene water --console 0:water tour --frames 880|368x448|nopin
stress-open|--quarter 1 --no-hud --scene stress-open --frames 2|448x368|nopin
stress-stack|--quarter 1 --no-hud --scene stress-stack --frames 2|448x368|nopin
stress-tiny-copies|--quarter 1 --no-hud --scene stress-tiny-copies --frames 2|448x368|nopin
stress-big-tris|--quarter 1 --no-hud --scene stress-big-tris --frames 2|448x368|nopin
stress-per-frame|--quarter 1 --no-hud --scene stress-per-frame --frames 2|448x368|nopin
stress-two-sided|--quarter 1 --no-hud --scene stress-two-sided --frames 2|448x368|nopin
stress-noisy-tiny|--quarter 1 --no-hud --scene stress-noisy-tiny --frames 2|448x368|nopin
"

# The scenes read their meshes from asset packs of the baked meshes in the tree.
scene_assets=main

# shellcheck source=../../../../tools/render/render_scene.sh
. "$SCRIPT_DIR/../../../../tools/render/render_scene.sh"
render_scene_run "$@"
