# The shell and the board drivers. Apps are NOT listed here - see below.
set(app_srcs
    "main.c"
    "boot/boot_anim.c"
    "boot/post.c"
    "boot/post_layout.c"
    "boot/post_ui.c"
    "display/display.c"
    "display/panel_clock.c"
    "gfx/gfx.c"
    "input/touch.c"
    "input/touch_fsm.c"
    "input/button_fsm.c"
    "input/buttons.c"
    "input/gesture.c"
    "input/imu.c"
    "ui/ui.c"
    "ui/system_navigation.c"
    "ui/ui_build.c"
    "ui/ui_control_center.c"
    "ui/ui_control_center_draw.c"
    "ui/ui_launcher.c"
    "ui/ui_launcher_draw.c"
    "ui/ui_pointer.c"
    "ui/ui_scroll.c"
    "util/device_state.c"
    "util/job.c"
    "util/screenshot.c"
)

if(CONFIG_LAUNCHER_QEMU)
    list(APPEND app_srcs "gfx/gfx_null_panel.c")
endif()

# board/board.h binds this target's hardware facts to board_esp32s3.c.
list(APPEND app_srcs "board/board_esp32s3.c")

# Apps are discovered, not listed.
#
# Everything an app owns lives in main/apps/<name>/ - its entry point, any
# logic of its own, and its test suite. Adding an app means creating the
# folder; deleting one means deleting the folder. Neither touches this file,
# and neither touches main.c, because apps register themselves (see app.h).
#
# CONFIGURE_DEPENDS makes CMake re-glob when the folder contents change, so a
# new app is picked up without a manual reconfigure.
#
# It cannot be used unconditionally. ESP-IDF evaluates this file twice: once in
# script mode (cmake -P) to work out component requirements, and once for real.
# CONFIGURE_DEPENDS is illegal in script mode and hard-errors the configure.
# The script pass only cares about REQUIRES, so plain globbing is fine there.
# (This is the same early pass that makes a Kconfig-gated REQUIRES silently
# fail - see the note on unity below.)
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE discovered_apps "${CMAKE_CURRENT_LIST_DIR}/apps/*.c")
else()
    file(GLOB_RECURSE discovered_apps CONFIGURE_DEPENDS
         "${CMAKE_CURRENT_LIST_DIR}/apps/*.c")
endif()

# apps/<name>/tools/ holds that app's own host-side tooling (sweep scripts,
# report generators) - see docs/Building-an-App.md, "An app is a
# folder". It is not firmware. But the glob above is apps/*.c, recursive, so
# a future .c placed under one of those tools/ folders (a small host-side
# helper program, say) would be swept in right alongside the app's real
# sources - and then WHOLE_ARCHIVE, below, force-links it into the image
# regardless of whether anything references it, so --gc-sections never gets
# the chance to drop it. Excluded by folder rather than by name, same as the
# diagnostics rule below, so the rule is structural rather than needing a
# per-tool update. Nothing lives here yet; this guard goes in with the
# convention itself, because the failure it prevents - a host tool's main()
# silently linked into device firmware - is silent and baffling to debug
# after the fact.
#
# Filtered HERE, before the suite split below, and not after it:
# app_suites is captured from discovered_apps and would otherwise keep
# a suite_*.c living under a tools/ folder, putting it back into the
# selftest image through the other door.
list(FILTER discovered_apps EXCLUDE REGEX "/apps/[^/]*/tools/")

# Test suites are app-owned but must not ship. They are added back below when
# CONFIG_LAUNCHER_SELFTEST is set.
set(app_suites ${discovered_apps})
list(FILTER app_suites INCLUDE REGEX "/suite_[^/]*\\.c$")
list(FILTER discovered_apps EXCLUDE REGEX "/suite_[^/]*\\.c$")

# Diagnostics carries the gfx debug-overlay checkboxes a developer wants
# while working on sand, plus the POST report and (SELFTEST builds only -
# see app_diagnostics.c) the self-test runner button. None of that belongs
# in a release image: entering the app re-runs POST, which cycles the audio
# rail - reasonable while debugging, not something to leave reachable in a
# shipped product. Gated
# on DEVELOPMENT rather than SELFTEST so it ships in a --dev build too, with
# no test suites along for the ride - that is the whole reason a --dev
# build exists: SELFTEST's linked-in suites cost enough static RAM that
# sand's grid allocation can fail. LAUNCHER_SELFTEST `select`s
# LAUNCHER_DEVELOPMENT (Kconfig.projbuild), so this one condition still
# covers both --dev and --diag. Excluded by folder rather than by name so
# the rule is structural.
if(NOT CONFIG_LAUNCHER_DEVELOPMENT)
    list(FILTER discovered_apps EXCLUDE REGEX "/apps/diagnostics/")
endif()

list(APPEND app_srcs ${discovered_apps})

if(CONFIG_LAUNCHER_SELFTEST)
    set(build_id_variant diag)
elseif(CONFIG_LAUNCHER_DEVELOPMENT)
    set(build_id_variant dev)
else()
    set(build_id_variant release)
endif()

set(build_id_generated_dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(NOT CMAKE_SCRIPT_MODE_FILE)
    file(MAKE_DIRECTORY "${build_id_generated_dir}")
    add_custom_target(build_id_header ALL
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_LIST_DIR}/../..
            -DBUILD_DIR=${CMAKE_BINARY_DIR}
            -DGENERATED_DIR=${build_id_generated_dir}
            -DVARIANT=${build_id_variant}
            -P ${CMAKE_CURRENT_LIST_DIR}/build_id.cmake
        BYPRODUCTS "${build_id_generated_dir}/build_id_generated.h"
        VERBATIM)
endif()

# post.c reaches for chip identity, flash size, I2C probing and the on-die
# temperature sensor, so it pulls in more of the platform than the rest.
#
# The SD card is native 1-bit SDMMC on its own pins (esp_driver_sdmmc), not
# SPI2. "sdmmc" itself is still needed for sdmmc_card_t and the protocol
# layer underneath the host driver.
#
# esp_lcd_co5300 brings up the V2 (CO5300 panel) revision - see gfx.c. It is
# a PRIVATE dependency of the BSP's own idf_component.yml, so main needs its
# own declaration (main/idf_component.yml) to see its headers.
set(app_reqs microui small3dlib esp_lcd heap esp_timer
             esp_driver_i2c esp_driver_tsens esp_hw_support spi_flash
             fatfs esp_driver_sdmmc sdmmc
             esp_driver_usb_serial_jtag vfs nvs_flash
             waveshare__esp_lcd_sh8601 espressif__esp_lcd_touch espressif__esp_lcd_co5300
             # perfmon and unity are both listed unconditionally, NOT inside
             # the block below: ESP-IDF expands REQUIRES in an early pass
             # where CONFIG_* is not yet defined, so a Kconfig-gated REQUIRES
             # silently does nothing - SRCS and compile definitions are
             # evaluated later and do work, which makes the failure look
             # baffling: the code compiles but cannot find the header.
             #
             # This costs release nothing. IDF puts both in the component
             # graph either way; REQUIRES only decides whether main can see
             # their headers. With nothing built that references them,
             # --gc-sections drops them from the image.
             perfmon unity)

if(CONFIG_LAUNCHER_SELFTEST)
    # The suites are shared with the host runner - same sources, so what passes
    # on a laptop is the same set of assertions the board makes.
    set(suite_srcs
        "../test/suites/suite_touch_fsm.c"
        "../test/suites/suite_gesture.c"
        "../test/suites/suite_button_fsm.c"
        "../test/suites/suite_rng.c"
        "../test/suites/suite_boot_anim.c"
        "../test/suites/suite_r3d_project.c"
        "../test/suites/suite_boot_anim_perf.c"
        "../test/suites/suite_fixed.c"
        "../test/suites/suite_tween.c"
        "../test/suites/suite_gfx.c"
        "../test/suites/suite_gfx_dirty.c"
        "../test/suites/suite_gfx_heal.c"
        "../test/suites/suite_gfx_color.c"
        "../test/suites/suite_gfx_font.c"
        "../test/suites/suite_gfx_font_roles.c"
        "../test/suites/suite_icons.c"
        "../test/suites/suite_icons_system.c"
        "../test/suites/suite_ui.c"
        "../test/suites/suite_ui_style.c"
        "../test/suites/suite_ui_transform.c"
        "../test/suites/suite_ui_anchor.c"
        "../test/suites/suite_control_center_layout.c"
        "../test/suites/suite_ui_centered_rect.c"
        "../test/suites/suite_ui_pointer.c"
        "../test/suites/suite_ui_pointer_microui.c"
        "../test/suites/suite_ui_scroll.c"
        "../test/suites/suite_ui_slider.c"
        "../test/suites/suite_display.c"
        "../test/suites/suite_post_ui.c"
        "../test/suites/suite_panel_clock.c"
        "../test/suites/suite_screenshot.c"
        "../test/suites/suite_build_id.c"
        "../test/suites/suite_device_state.c"
        "../test/suites/suite_job.c"
    )
    # SCOPE: which of those suites this image actually carries.
    #
    # Full is the default and stays globbed, so nothing can lose coverage by
    # accident - a scope only ever NARROWS, and only when asked for by name
    # (CONFIG_LAUNCHER_SELFTEST_SCOPE_*, Kconfig.projbuild).
    #
    # The perf scope is a literal list rather than a pattern because it is
    # shaped around what a capture READS, and that does not follow a folder
    # or a filename: suite_sand_perf.c's rows are built by the scene
    # builders in suite_sand_scenes.c and the fixtures in suite_sand_common.c,
    # neither of which is named _perf, while suite_cube_perf.c and
    # suite_boot_anim_perf.c are named _perf and are read by nobody in a sand
    # round. Explicit, like the shell suite list above it.
    #
    # A member that goes missing (renamed, deleted with its app) fails the
    # CONFIGURE below rather than quietly shrinking the scope. A member that
    # is still here but no longer sufficient - an in-scope suite calling a
    # builder from a file this list leaves out - fails at LINK, because the
    # builders are ordinary called symbols and nothing stubs them.
    if(CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF)
        set(app_suites
            "${CMAKE_CURRENT_LIST_DIR}/apps/sand/suite_sand_perf.c"
            "${CMAKE_CURRENT_LIST_DIR}/apps/sand/suite_sand_scenes.c"
            "${CMAKE_CURRENT_LIST_DIR}/apps/sand/suite_sand_common.c"
        )
        foreach(suite_src IN LISTS app_suites)
            if(NOT EXISTS "${suite_src}")
                message(FATAL_ERROR
                    "LAUNCHER_SELFTEST_SCOPE_PERF names a suite that does not "
                    "exist: ${suite_src}. Fix the list in main/CMakeLists.txt "
                    "(and docs/Testing-Guide.md) - a scope that silently "
                    "measures less than it claims is worse than no scope.")
            endif()
        endforeach()
        set(suite_srcs "")
    endif()

    list(APPEND app_srcs
        "boot/selftest.c"
        "../test/suites.c"
        "../test/timing.c"
        ${suite_srcs}
        ${app_suites}
    )

    # Every suite calls RUN_TEST(func) directly; timing.h intercepts that
    # macro (see its own comment) to log a wall-clock line per test without
    # editing a single suite file. -include forces it in ahead of each
    # suite's own "#include unity.h", which is what lets Unity's own
    # "#ifndef RUN_TEST" step aside for it. Scoped to just the suite files -
    # forcing it onto every source in this component would be harmless too
    # (nothing else calls RUN_TEST), but there's no reason to.
    set_source_files_properties(${suite_srcs} ${app_suites} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/../test/timing.h")
endif()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "." "../test" "${build_id_generated_dir}"
    REQUIRES ${app_reqs}

    # Required by self-registration, and easy to get wrong.
    #
    # This component becomes libmain.a, and a linker only extracts an archive
    # member if it resolves an undefined symbol. Nothing references an app by
    # name any more - that is the entire point - so app_sand.o would never be
    # extracted, its constructor would never enter .init_array, and the app
    # would silently vanish from the menu. Not a link error: a smaller binary
    # and a shorter list.
    #
    # WHOLE_ARCHIVE links every member regardless. Section garbage collection
    # still runs, so genuinely unreachable code is still dropped.
    WHOLE_ARCHIVE
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    add_dependencies(${COMPONENT_LIB} build_id_header)
endif()

if(CONFIG_LAUNCHER_SELFTEST)
    # Tells the shared suite headers they are being built for the target, which
    # is what enables the hardware-only suites.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BUILD=1)
endif()
