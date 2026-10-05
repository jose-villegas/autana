# The shell and the board drivers. Apps are NOT listed here; see below.
set(app_srcs
    "main.c"
    "app_arena.c"
    "app_registry.c"
    "boot/boot_anim.c"
    "boot/boot_anim_tracks_generated.c"
    "boot/post.c"
    "boot/post_layout.c"
    "boot/post_ui.c"
    "display/display.c"
    "display/display_device.c"
    "display/panel_clock.c"
    "gfx/gfx.c"
    "input/touch.c"
    "input/touch_calib.c"
    "input/touch_point.c"
    "input/touch_fsm.c"
    "input/button_fsm.c"
    "input/buttons.c"
    "input/gesture.c"
    "input/imu.c"
    "input/input_device.c"
    "input/tilt.c"
    "ui/ui.c"
    "ui/system_navigation.c"
    "ui/ui_bridge.c"
    "ui/ui_build.c"
    "ui/ui_canvas_marks.c"
    "ui/ui_control_center.c"
    "ui/ui_control_center_draw.c"
    "ui/ui_launcher.c"
    "ui/ui_launcher_draw.c"
    "ui/ui_pointer.c"
    "ui/ui_snap.c"
    "ui/ui_ridge.c"
    "ui/ui_scroll.c"
    "ui/ui_widgets.c"
    "anim/anim_track.c"
    "anim/anim_tracks.c"
    "asset/asset_pack.c"
    "asset/asset_store_flash.c"
    "render/raster.c"
    "render/r3d_lit_mesh.c"
    "render/r3d_pipeline.c"
    "render/upscale.c"
    "render/r3d_span.c"
    "render/r3d_scene.c"
    "scene/scene.c"
    "scene/scene_draw.c"
    "scene/scene_shell.c"
    "shell/scene_system.c"
    "shell/shell.c"
    "shell/shell_apps.c"
    "shell/shell_system.c"
    "util/frame_cost.c"
    "util/frame_watch.c"
    "util/build_id.c"
    "util/job.c"
    "util/memory.c"
    "util/settings_device.c"
    "util/settings_policy.c"
    "util/timing_device.c"
    "util/tune.c"
)

function(launcher_glob output pattern)
    if(CMAKE_SCRIPT_MODE_FILE)
        file(GLOB_RECURSE files "${pattern}")
    else()
        file(GLOB_RECURSE files CONFIGURE_DEPENDS "${pattern}")
    endif()
    set(${output} ${files} PARENT_SCOPE)
endfunction()

if(CONFIG_LAUNCHER_QEMU)
    list(APPEND app_srcs "gfx/gfx_null_panel_device.c")
endif()

# The frame's view modes: a development tool, with nothing calling them in a
# release build.
if(CONFIG_LAUNCHER_DEVELOPMENT)
    list(APPEND app_srcs "render/raster_show.c")
endif()

# The console listener: a release image has nobody watching the serial
# console to type a verb into (see console/console.h), so a release build
# compiles none of console/*.c: no reader task, no registry, no verbs.
# RUNSUITE and TOUCH/IMU narrow further still, to the one build variant
# each actually answers on.
if(CONFIG_LAUNCHER_DEVELOPMENT)
    list(APPEND app_srcs
        "console/console_verbs.c"
        "console/console.c"
        "console/console_buildid.c"
        "console/console_freeze.c"
        "console/console_frame_watch.c"
        "console/console_navigation.c"
        "console/console_perf.c"
        "console/console_tune.c"
        "console/console_screenshot.c"
        "console/device_state.c"
    )
endif()
if(CONFIG_LAUNCHER_SELFTEST)
    list(APPEND app_srcs "console/console_runsuite.c")
endif()
if(CONFIG_LAUNCHER_DEVELOPMENT)
    list(APPEND app_srcs
        "console/console_inject.c"
        "input/touch_inject_fsm.c"
        "input/touch_gesture.c"
    )
endif()

# board/board.h binds this target's hardware facts to board_esp32s3.c.
list(APPEND app_srcs "board/board_esp32s3.c")

# Apps are discovered, not listed.
#
# Everything an app owns lives in main/apps/<name>/: its entry point, any
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
# fail; see the note on unity below.)
launcher_glob(discovered_apps "${CMAKE_CURRENT_LIST_DIR}/apps/*.c")

# apps/<name>/tools/ is an app's host-side tooling, not firmware. The glob
# above is recursive and WHOLE_ARCHIVE force-links whatever it finds, so a
# host program's main() placed there would silently land in the image.
# Filtered before the suite split below, which would otherwise keep a
# suite_*.c from a tools/ folder.
list(FILTER discovered_apps EXCLUDE REGEX "/apps/[^/]*/tools/")

# Test suites are app-owned but must not ship. They are added back below when
# CONFIG_LAUNCHER_SELFTEST is set.
set(app_suites ${discovered_apps})
list(FILTER app_suites INCLUDE REGEX "/suite_[^/]*\\.c$")
list(FILTER discovered_apps EXCLUDE REGEX "/suite_[^/]*\\.c$")

# An app whose folder holds a development_only.cmake is a bench tool: it
# ships in any DEVELOPMENT build (--dev and --diag, since SELFTEST depends on
# DEVELOPMENT) and never in release. The app declares it from its own folder,
# so deleting the folder deletes the declaration.
if(NOT CONFIG_LAUNCHER_DEVELOPMENT)
    launcher_glob(development_only_markers
        "${CMAKE_CURRENT_LIST_DIR}/apps/*/development_only.cmake")
    list(FILTER development_only_markers INCLUDE REGEX
        "/apps/[^/]+/development_only\\.cmake$")
    foreach(marker IN LISTS development_only_markers)
        get_filename_component(app_dir "${marker}" DIRECTORY)
        get_filename_component(app "${app_dir}" NAME)
        list(FILTER discovered_apps EXCLUDE REGEX "/apps/${app}/")
    endforeach()
endif()

list(APPEND app_srcs ${discovered_apps})

# post.c reaches for chip identity, flash size, I2C probing and the on-die
# temperature sensor, so it pulls in more of the platform than the rest.
#
# The SD card is native 1-bit SDMMC on its own pins (esp_driver_sdmmc), not
# SPI2. "sdmmc" itself is still needed for sdmmc_card_t and the protocol
# layer underneath the host driver.
#
# esp_lcd_co5300 brings up the V2 (CO5300 panel) revision; see gfx.c. It is
# a PRIVATE dependency of the BSP's own idf_component.yml, so main needs its
# own declaration (main/idf_component.yml) to see its headers.
set(app_reqs microui esp32_s3_touch_amoled_1_8 esp_lcd heap esp_timer
             esp_driver_i2c esp_driver_tsens esp_hw_support esp_psram spi_flash esp_partition
             fatfs esp_driver_sdmmc sdmmc
             esp_driver_usb_serial_jtag vfs nvs_flash esp_app_format
             waveshare__esp_lcd_sh8601 espressif__esp_lcd_touch espressif__esp_lcd_co5300
             # perfmon and unity are both listed unconditionally, NOT inside
             # the block below: ESP-IDF expands REQUIRES in an early pass
             # where CONFIG_* is not yet defined, so a Kconfig-gated REQUIRES
             # silently does nothing; SRCS and compile definitions are
             # evaluated later and do work, which makes the failure look
             # baffling: the code compiles but cannot find the header.
             #
             # This costs release nothing. IDF puts both in the component
             # graph either way; REQUIRES only decides whether main can see
             # their headers. With nothing built that references them,
             # --gc-sections drops them from the image.
             perfmon unity)

if(CONFIG_LAUNCHER_SELFTEST)
    launcher_glob(suite_srcs "${CMAKE_CURRENT_LIST_DIR}/../test/suites/suite_*.c")
    # SCOPE: full is the default and stays globbed, so nothing can lose
    # coverage by accident; a scope only ever NARROWS, and only when asked
    # for by name (CONFIG_LAUNCHER_SELFTEST_SCOPE_*, Kconfig.projbuild). The
    # perf scope replaces the shell suites outright with what apps declare.
    if(CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF)
        launcher_glob(perf_scope_files "${CMAKE_CURRENT_LIST_DIR}/apps/*/scope_perf.cmake")
        set(perf_scope_srcs "")
        foreach(perf_scope_file IN LISTS perf_scope_files)
            include("${perf_scope_file}")
        endforeach()
        if(NOT perf_scope_srcs)
            message(FATAL_ERROR "LAUNCHER_SELFTEST_SCOPE_PERF has no sources")
        endif()
        foreach(suite_src IN LISTS perf_scope_srcs)
            if(NOT EXISTS "${suite_src}")
                message(FATAL_ERROR
                    "LAUNCHER_SELFTEST_SCOPE_PERF names a missing source: "
                    "${suite_src}")
            endif()
        endforeach()
        set(app_suites ${perf_scope_srcs})
        set(suite_srcs "")
    endif()

    list(APPEND app_srcs
        "boot/selftest.c"
        "../test/suites.c"
        "../test/timing.c"
        "../test/test_cleanup.c"
        ${suite_srcs}
        ${app_suites}
    )

    # Every suite calls RUN_TEST(func) directly; timing.h intercepts that
    # macro (see its own comment) to log a wall-clock line per test without
    # editing a single suite file. -include forces it in ahead of each
    # suite's own "#include unity.h", which is what lets Unity's own
    # "#ifndef RUN_TEST" step aside for it. Scoped to just the suite files;
    # forcing it onto every source in this component would be harmless too
    # (nothing else calls RUN_TEST), but there's no reason to.
    set_source_files_properties(${suite_srcs} ${app_suites} PROPERTIES
        COMPILE_OPTIONS "-include;${CMAKE_CURRENT_LIST_DIR}/../test/timing.h")
endif()

# The software rasteriser's per-pixel loops run from flash through the
# instruction cache, so where each function starts inside a cache line moves
# the frame time by a fraction of a percent whenever unrelated code ahead of
# it in the link grows or shrinks. Starting every render function on a cache
# line makes its timing independent of link order.
if(NOT CMAKE_SCRIPT_MODE_FILE)
    launcher_glob(render_srcs "${CMAKE_CURRENT_LIST_DIR}/render/*.c")
    set_source_files_properties(${render_srcs} PROPERTIES
        COMPILE_OPTIONS "-falign-functions=${CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE}")
endif()

# LAUNCHER_LAYOUT_SEED > 0 pads every source of this component by a size its
# seed picks (tools/build/layout_pad.py), up to one cache way less a line each
# for code and flash rodata, to sample the layouts a timing could have landed
# on. Seed 0 changes nothing.
if(NOT CMAKE_SCRIPT_MODE_FILE AND LAUNCHER_LAYOUT_SEED)
    idf_build_get_property(layout_python PYTHON)
    set(layout_sources "")
    foreach(src IN LISTS app_srcs)
        get_filename_component(src "${src}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_LIST_DIR}")
        file(RELATIVE_PATH src "${CMAKE_CURRENT_LIST_DIR}" "${src}")
        list(APPEND layout_sources "${src}")
    endforeach()
    set(layout_tools "${CMAKE_CURRENT_LIST_DIR}/../tools/build")
    set(text_line ${CONFIG_ESP32S3_INSTRUCTION_CACHE_LINE_SIZE})
    set(rodata_line ${CONFIG_ESP32S3_DATA_CACHE_LINE_SIZE})
    math(EXPR text_way "${CONFIG_ESP32S3_INSTRUCTION_CACHE_SIZE} / ${CONFIG_ESP32S3_ICACHE_ASSOCIATED_WAYS}")
    math(EXPR rodata_way "${CONFIG_ESP32S3_DATA_CACHE_SIZE} / ${CONFIG_ESP32S3_DCACHE_ASSOCIATED_WAYS}")
    execute_process(
        COMMAND ${layout_python} "${layout_tools}/layout_pad.py" "${LAUNCHER_LAYOUT_SEED}"
                ${text_line} ${text_way} ${rodata_line} ${rodata_way} ${layout_sources}
        OUTPUT_VARIABLE layout_pads
        COMMAND_ERROR_IS_FATAL ANY)
    foreach(row IN LISTS layout_pads)
        string(REPLACE "," ";" row "${row}")
        list(GET row 0 src)
        list(GET row 1 text_pad)
        list(GET row 2 rodata_pad)
        set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/${src}" APPEND PROPERTY COMPILE_OPTIONS
            "-include;${layout_tools}/layout_pad.h;-DLAYOUT_PAD_TEXT=${text_pad};-DLAYOUT_PAD_RODATA=${rodata_pad};-DLAYOUT_PAD_TEXT_ALIGN=${text_line};-DLAYOUT_PAD_RODATA_ALIGN=${rodata_line}")
    endforeach()
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${layout_tools}/layout_pad.py")
endif()

set(app_private_include_dirs "")
if(CONFIG_LAUNCHER_SELFTEST)
    list(APPEND app_private_include_dirs "../test")
endif()

idf_component_register(
    SRCS ${app_srcs}
    INCLUDE_DIRS "."
    PRIV_INCLUDE_DIRS ${app_private_include_dirs}
    LDFRAGMENTS "linker.lf"
    REQUIRES ${app_reqs}

    # Required by self-registration, and easy to get wrong.
    #
    # This component becomes libmain.a, and a linker only extracts an archive
    # member if it resolves an undefined symbol. Nothing references an app by
    # name any more (that is the entire point), so app_sand.o would never be
    # extracted, its constructor would never enter .init_array, and the app
    # would silently vanish from the menu. Not a link error: a smaller binary
    # and a shorter list.
    #
    # WHOLE_ARCHIVE links every member regardless. Section garbage collection
    # still runs, so genuinely unreachable code is still dropped.
    WHOLE_ARCHIVE
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=vla -Werror=unused-macros)

    # The FPU has no double: a float promoted to one, or a double constant in
    # float maths, runs in software at an order of magnitude the cost. Both
    # are compile errors across the firmware, and the link gate
    # (tools/build/check_no_soft_double.py) names any soft-double routine the
    # image still pulls in.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=double-promotion -Werror=float-conversion)

    # The asset pack is a build product: every baked mesh the import and scene
    # files name and every clip an .anim.toml names, packed into one image that
    # flashes to the "assets" partition with the app. build_pack.py searches
    # all of main/, so its inputs are globbed there too: a clip's .glb counts.
    # `idf.py assets-flash` writes just that region.
    idf_build_get_property(python PYTHON)
    set(asset_pack "${CMAKE_BINARY_DIR}/assets.bin")
    set(asset_tools "${CMAKE_CURRENT_LIST_DIR}/../tools")
    launcher_glob(asset_inputs "${CMAKE_CURRENT_LIST_DIR}/*.mesh")
    launcher_glob(asset_settings "${CMAKE_CURRENT_LIST_DIR}/*.toml")
    launcher_glob(asset_sources "${CMAKE_CURRENT_LIST_DIR}/*.glb")
    add_custom_command(
        OUTPUT "${asset_pack}"
        COMMAND ${python} "${asset_tools}/r3d/build_pack.py" -o "${asset_pack}" "${CMAKE_CURRENT_LIST_DIR}"
        DEPENDS ${asset_inputs} ${asset_settings} ${asset_sources} "${asset_tools}/r3d/build_pack.py"
                "${asset_tools}/r3d/import_settings.py" "${asset_tools}/r3d/mesh_asset.py"
                "${asset_tools}/asset/asset_pack.py" "${asset_tools}/anim/tracks_asset.py"
                "${asset_tools}/gltf/gltf_read.py"
        COMMENT "Packing the asset pack"
        VERBATIM)
    add_custom_target(asset_pack_image ALL DEPENDS "${asset_pack}")
    esptool_py_flash_to_partition(flash "assets" "${asset_pack}")
    add_dependencies(flash asset_pack_image)
    partition_table_get_partition_info(asset_offset "--partition-name assets" "offset")
    idf_component_get_property(flash_main_args esptool_py FLASH_ARGS)
    idf_component_get_property(flash_sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(assets-flash "${flash_main_args}" "${flash_sub_args}")
    esptool_py_flash_target_image(assets-flash assets "${asset_offset}" "${asset_pack}")
    add_dependencies(assets-flash asset_pack_image)
endif()

if(CONFIG_LAUNCHER_SELFTEST)
    # Tells the shared suite headers they are being built for the target, which
    # is what enables the hardware-only suites.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BUILD=1)
    # Unity's ROM output bypasses the app's USB serial console.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_system_console_put_char")
endif()
