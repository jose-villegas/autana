# The shell and the board drivers. Apps are NOT listed here; see below.
set(app_srcs
    "main.c"
    "sound/sound_mixer.c"
    "sound/sound_mixer_device.c"
    "audio/audio_codec.c"
    "audio/audio_out.c"
    "audio/audio_in.c"
    "audio/audio_tone.c"
    "app/app_arena.c"
    "app/app_registry.c"
    "boot/boot_anim.c"
    "boot/boot_anim_motion.c"
    "boot/boot_anim_photo.c"
    "boot/boot_report.c"
    "selftest/post.c"
    "selftest/post_layout.c"
    "selftest/post_ui.c"
    "display/display.c"
    "display/display_device.c"
    "display/panel_clock.c"
    "draw/draw.c"
    "draw/draw_image.c"
    "draw/icon.c"
    "gfx/present/gfx_debug.c"
    "gfx/present/expand/gfx_expand.c"
    "gfx/present/expand/gfx_quarter_select.c"
    "gfx/present/gfx_mode.c"
    "gfx/present/modes/indexed/gfx_indexed.c"
    "gfx/present/gfx_present.c"
    "input/touch/touch.c"
    "input/motion/imu_rest.c"
    "input/motion/imu_calib.c"
    "input/touch/touch_calib.c"
    "input/touch/touch_fsm.c"
    "input/buttons/button_fsm.c"
    "input/buttons/buttons.c"
    "input/touch/gesture.c"
    "input/motion/imu.c"
    "input/motion/imu_queue.c"
    "power/power_decode.c"
    "power/power_cache.c"
    "power/power_log.c"
    "power/power_log_device.c"
    "power/power_device.c"
    "input/input_device.c"
    "input/motion/motion_reader.c"
    "input/motion/gravity.c"
    "input/motion/shake.c"
    "ui/ui.c"
    "ui/input/ui_bridge.c"
    "ui/ui_build.c"
    "ui/face/ui_face.c"
    "ui/ui_canvas_marks.c"
    "ui/controls/ui_icons.c"
    "shell/ui/ui_launcher.c"
    "shell/ui/ui_launcher_draw.c"
    "ui/input/ui_pointer.c"
    "ui/input/ui_snap.c"
    "shell/ui/ui_ridge.c"
    "ui/controls/ui_widgets.c"
    "sound/sound.c"
    "audio/audio_adpcm.c"
    "asset/asset_pack.c"
    "asset/asset_table.c"
    "asset/asset_directory.c"
    "asset/asset_store.c"
    "asset/asset_store_async.c"
    "asset/asset_store_flash.c"
    "input/mapping/orbit_motion_touch.c"
    "shell/scene_system.c"
    "shell/shell.c"
    "shell/shell_apps.c"
    "shell/shell_home_edge.c"
    "shell/shell_frame.c"
    "shell/shell_system.c"
    "core/pending.c"
    "shell/asset_system.c"
    "shell/pending_system.c"
    "core/job.c"
    "core/memory.c"
    "core/timing_device.c"
    "profile/frame_cost.c"
    "profile/frame_laps.c"
    "profile/frame_watch.c"
    "services/build_id.c"
    "services/settings_device.c"
    "services/settings_policy.c"
    "core/tune.c"
)

function(launcher_glob output)
    if(CMAKE_SCRIPT_MODE_FILE)
        file(GLOB_RECURSE files ${ARGN})
    else()
        file(GLOB_RECURSE files CONFIGURE_DEPENDS ${ARGN})
    endif()
    set(${output} ${files} PARENT_SCOPE)
endfunction()

if(CONFIG_LAUNCHER_QEMU)
    list(APPEND app_srcs "gfx/present/panel/gfx_null_panel_device.c")
endif()

# The renderer: every .c below the folders renderer_folders.txt names, the
# list host builds read too (tools/render/CMakeLists.txt, tools/build/renderer.sh, renderer.py).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/renderer_folders.txt" renderer_folders REGEX "^[^#]")
foreach(folder IN LISTS renderer_folders)
    launcher_glob(folder_srcs "${CMAKE_CURRENT_LIST_DIR}/${folder}/*.c")
    list(APPEND app_srcs ${folder_srcs})
endforeach()
if(NOT CMAKE_SCRIPT_MODE_FILE)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/renderer_folders.txt")
endif()

# The frame's view modes, everything in render/view/: a development tool, with
# nothing calling them in a release build.
if(NOT CONFIG_LAUNCHER_DEVELOPMENT)
    list(FILTER app_srcs EXCLUDE REGEX "/render/view/[^/]*\.c$")
endif()

# The console listener: a release image has nobody watching the serial
# console to type a verb into (see console/console.h), so a release build
# compiles none of it: no reader task, no registry, no verbs. A verb lives in
# a console/ folder beside the module it drives, so adding one never touches
# this file. RUNSUITE narrows further, to the self-test build that answers it.
if(CONFIG_LAUNCHER_DEVELOPMENT)
    launcher_glob(console_srcs
        "${CMAKE_CURRENT_LIST_DIR}/console/*.c"
        "${CMAKE_CURRENT_LIST_DIR}/*/console/*.c")
    list(APPEND app_srcs ${console_srcs} "input/touch/touch_inject_fsm.c")
    # Acoustic detection belongs to the development console.
    list(APPEND app_srcs
        "audio/audio_loopback.c"
        "audio/audio_loopback_detector.c"
        "audio/audio_frequency.c"
    )
endif()
if(CONFIG_LAUNCHER_SELFTEST)
    list(APPEND app_srcs "selftest/console_runsuite.c")
endif()

# board/board.h binds this target's hardware facts to board_esp32s3.c.
list(APPEND app_srcs "board/board_esp32s3.c" "board/board_panel.c")
# At 120 MHz, PSRAM tuning runs with the USB port off the bus; the wrap that
# routes it through board_usb.c is added after idf_component_register below.
if(CONFIG_SPIRAM_SPEED_120M)
    list(APPEND app_srcs "board/board_usb.c")
endif()

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
set(app_roots "${CMAKE_CURRENT_LIST_DIR}/apps" "${CMAKE_CURRENT_LIST_DIR}/shell/apps")
set(discovered_apps "")
foreach(app_root IN LISTS app_roots)
    launcher_glob(root_apps "${app_root}/*.c")
    list(APPEND discovered_apps ${root_apps})
endforeach()

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
    set(development_only_markers "")
    foreach(app_root IN LISTS app_roots)
        launcher_glob(root_markers "${app_root}/*/development_only.cmake")
        list(APPEND development_only_markers ${root_markers})
    endforeach()
    list(FILTER development_only_markers INCLUDE REGEX
        "/apps/[^/]+/development_only\\.cmake$")
    foreach(marker IN LISTS development_only_markers)
        get_filename_component(app_dir "${marker}" DIRECTORY)
        get_filename_component(app "${app_dir}" NAME)
        list(FILTER discovered_apps EXCLUDE REGEX "/apps/${app}/")
    endforeach()
endif()

list(APPEND app_srcs ${discovered_apps})

# Every console/ folder is development-only, whichever glob above found it.
if(NOT CONFIG_LAUNCHER_DEVELOPMENT)
    list(FILTER app_srcs EXCLUDE REGEX "/console/[^/]*\.c$")
endif()

# post.c reaches for chip identity, flash size, I2C probing and the on-die
# temperature sensor, so it pulls in more of the platform than the rest.
#
# The SD card is native 1-bit SDMMC on its own pins (esp_driver_sdmmc), not
# SPI2. "sdmmc" itself is still needed for sdmmc_card_t and the protocol
# layer underneath the host driver.
#
# esp_lcd_co5300 brings up the V2 (CO5300 panel) revision; see board/board_panel.c. It is
# a PRIVATE dependency of the BSP's own idf_component.yml, so main needs its
# own declaration (main/idf_component.yml) to see its headers.
set(app_reqs esp_codec_dev esp_driver_i2s math simd kinematics physics image hash collections microui esp32_s3_touch_amoled_1_8 esp_lcd heap esp_timer
             esp_driver_i2c esp_driver_tsens esp_hw_support esp_psram spi_flash esp_partition
             fatfs esp_driver_sdmmc sdmmc
             esp_driver_usb_serial_jtag esp_driver_uart vfs nvs_flash esp_app_format espcoredump
             waveshare__esp_lcd_sh8601 espressif__esp_lcd_touch espressif__esp_lcd_co5300
             # perfmon, unity, testkit and esp_mm (a perf suite's cache control) are
             # listed unconditionally, NOT inside the block below: ESP-IDF
             # expands REQUIRES in an early pass
             # where CONFIG_* is not yet defined, so a Kconfig-gated REQUIRES
             # silently does nothing; SRCS and compile definitions are
             # evaluated later and do work, which makes the failure look
             # baffling: the code compiles but cannot find the header.
             #
             # This costs release nothing. IDF puts them in the component
             # graph either way; REQUIRES only decides whether main can see
             # their headers. With nothing built that references them,
             # --gc-sections drops them from the image.
             perfmon unity testkit esp_mm)

if(CONFIG_LAUNCHER_SELFTEST)
    launcher_glob(suite_srcs "${CMAKE_CURRENT_LIST_DIR}/../test/suites/suite_*.c"
        "${CMAKE_CURRENT_LIST_DIR}/../packages/*/tests/suite_*.c"
        "${CMAKE_CURRENT_LIST_DIR}/../packages/*/bench/suite_*.c")
    # SCOPE: full is the default and stays globbed, so nothing can lose
    # coverage by accident; a scope only ever NARROWS, and only when asked
    # for by name (CONFIG_LAUNCHER_SELFTEST_SCOPE_*, Kconfig.projbuild). The
    # perf scope includes only what engine and app manifests declare.
    if(CONFIG_LAUNCHER_SELFTEST_SCOPE_PERF)
        launcher_glob(perf_scope_files "${CMAKE_CURRENT_LIST_DIR}/../test/scope_perf.cmake")
        foreach(app_root IN LISTS app_roots)
            launcher_glob(root_scope_files "${app_root}/*/scope_perf.cmake")
            list(APPEND perf_scope_files ${root_scope_files})
        endforeach()
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
        "selftest/selftest.c"
        "../test/suites.c"
        "../test/timing.c"
        "../test/bench_env.c"
        "../test/frame_profile.c"
        "../test/ui_probe.c"
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

# Render code outside IRAM runs from flash through the instruction cache,
# so where each function starts inside a cache line moves
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

    # Constructors self-register entries without a referenced symbol pulling
    # their archive members into the link. Keep those constructors while
    # section garbage collection drops unreachable code.
    WHOLE_ARCHIVE
)

if(NOT CMAKE_SCRIPT_MODE_FILE)
    if(TUNE_HOT_LIVE)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE TUNE_HOT_LIVE=1)
    endif()
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=vla -Werror=unused-macros)

    # The FPU has no double: a float promoted to one, or a double constant in
    # float maths, runs in software at an order of magnitude the cost. Both
    # are compile errors across the firmware, and the link gate
    # (tools/build/check_no_soft_double.py) names any soft-double routine the
    # image still pulls in.
    target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=double-promotion -Werror=float-conversion)

    # Optimisation flags measured on the pinned GCC (launcher/idf-pin.json):
    # re-measure each on and off when its major version changes
    # (docs/tools/Framework-Versions.md).
    # Extension dead-code elimination, on at -O2, costs this component's hot
    # loops up to 2% by the frame-budget perf suites (GCC 15.2: packed gas
    # step 37.9 -> 37.2 ms, 20 px gathered present 241 -> 236 us without it).
    target_compile_options(${COMPONENT_LIB} PRIVATE -fno-ext-dce)
    # -O3 scheduling removes pipeline bubbles -O2 leaves in the skinning and
    # lighting kernels' float chains (GCC 15.2, suite_skin_light_perf, one
    # core: fused_direct_1 2692 -> 1906 us, fused_renormalised_1 2902 -> 2296).
    set_property(SOURCE "${CMAKE_CURRENT_LIST_DIR}/render/skin/r3d_skinning.c" APPEND PROPERTY COMPILE_OPTIONS "-O3")

    # The packer owns input discovery, including demo manifests, and runs on
    # every build so deleted manifests drop their demos. Unchanged packs and
    # the image retain their timestamps. The image flashes to the "assets"
    # partition; `idf.py assets-flash` writes just that region.
    idf_build_get_property(python PYTHON)
    set(asset_image "${CMAKE_BINARY_DIR}/assets.bin")
    set(asset_packs "${CMAKE_BINARY_DIR}/assets")
    set(asset_tools "${CMAKE_CURRENT_LIST_DIR}/../tools")
    set(asset_variant_args "")
    if(CONFIG_LAUNCHER_DEVELOPMENT)
        list(APPEND asset_variant_args --development)
    endif()
    add_custom_target(asset_pack_image ALL
        COMMAND ${python} "${asset_tools}/r3d/build_pack.py" -o "${asset_packs}" --image "${asset_image}"
                ${asset_variant_args} "${CMAKE_CURRENT_LIST_DIR}"
        BYPRODUCTS "${asset_image}"
        COMMENT "Writing the asset packs"
        VERBATIM)
    esptool_py_flash_to_partition(flash "assets" "${asset_image}")
    add_dependencies(flash asset_pack_image)
    partition_table_get_partition_info(asset_offset "--partition-name assets" "offset")
    idf_component_get_property(flash_main_args esptool_py FLASH_ARGS)
    idf_component_get_property(flash_sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(assets-flash "${flash_main_args}" "${flash_sub_args}")
    esptool_py_flash_target_image(assets-flash assets "${asset_offset}" "${asset_image}")
    add_dependencies(assets-flash asset_pack_image)
endif()

if(CONFIG_SPIRAM_SPEED_120M)
    # Routes ESP-IDF's PSRAM tuning through board/board_usb.c, so the USB port
    # is off the bus for it. tools/build/check_psram_tuning_wrap.py checks the
    # linked image.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=mspi_timing_psram_tuning")
endif()

if(CONFIG_LAUNCHER_SELFTEST)
    # Tells the shared suite headers they are being built for the target, which
    # is what enables the hardware-only suites.
    target_compile_definitions(${COMPONENT_LIB} PRIVATE DEVICE_BUILD=1)
    # Unity's ROM output bypasses the app's USB serial console.
    target_link_options(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_system_console_put_char")
endif()
