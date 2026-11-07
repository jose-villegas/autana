# The engine sources every host build compiles, the one list. Sets launcher
# (the launcher/ folder), render_engine_sources (drawing without device
# backends, the asset store, tune, jobs and pending requests), renderer_sources
# (main/renderer_sources.cmake) and, for CMakeLists.txt, which includes it,
# board_include_dir (main/board/board_dir.cmake).
#
# A build without CMake reads both lists as absolute paths on stdout, one per line:
#
#   cmake -P launcher/tools/render/engine_sources.cmake

get_filename_component(launcher "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
include("${launcher}/main/renderer_sources.cmake")
include("${launcher}/main/board/board_dir.cmake")

launcher_glob(render_engine_sources "${launcher}/main/gfx/*.c" "${launcher}/main/draw/*.c")
list(FILTER render_engine_sources EXCLUDE REGEX "_device\\.c$")
foreach(asset asset_pack asset_table asset_file asset_store asset_store_async asset_store_file)
    list(APPEND render_engine_sources "${launcher}/main/asset/${asset}.c")
endforeach()
list(APPEND render_engine_sources
     "${launcher}/main/core/tune.c" "${launcher}/main/console/console_tune.c"
     "${launcher}/main/core/job.c" "${launcher}/main/core/pending.c"
     "${launcher}/main/core/app_heap.c"
     "${launcher}/main/app/app_arena.c"
     "${launcher}/../third_party/tlsf/tlsf.c")
list(SORT render_engine_sources)

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    string(JOIN "\n" engine_listing ${render_engine_sources} ${renderer_sources})
    execute_process(COMMAND "${CMAKE_COMMAND}" -E echo "${engine_listing}")
endif()
