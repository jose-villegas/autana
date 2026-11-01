# The engine sources every host build compiles, the one list: drawing (no
# device backends), the asset store, tune and jobs in render_engine_sources,
# and every .c below the folders main/renderer_folders.txt names in
# renderer_sources. CMakeLists.txt includes it; a build without CMake reads
# the same list as absolute paths, one per line:
#
#   cmake -P launcher/tools/render/engine_sources.cmake

get_filename_component(launcher "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
if(CMAKE_SCRIPT_MODE_FILE)
    set(configure_depends "")
else()
    set(configure_depends CONFIGURE_DEPENDS)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${launcher}/main/renderer_folders.txt")
endif()

file(STRINGS "${launcher}/main/renderer_folders.txt" renderer_folders REGEX "^[^#]")
set(renderer_sources "")
foreach(folder IN LISTS renderer_folders)
    string(STRIP "${folder}" folder)
    if(NOT folder STREQUAL "")
        file(GLOB_RECURSE folder_sources ${configure_depends} "${launcher}/main/${folder}/*.c")
        list(APPEND renderer_sources ${folder_sources})
    endif()
endforeach()
list(SORT renderer_sources)

file(GLOB_RECURSE render_engine_sources ${configure_depends}
     "${launcher}/main/gfx/*.c" "${launcher}/main/draw/*.c")
list(FILTER render_engine_sources EXCLUDE REGEX "_device\\.c$")
foreach(asset asset_pack asset_table asset_file asset_store asset_store_file)
    list(APPEND render_engine_sources "${launcher}/main/asset/${asset}.c")
endforeach()
list(APPEND render_engine_sources
     "${launcher}/main/core/tune.c" "${launcher}/main/console/console_tune.c"
     "${launcher}/main/core/job.c")
list(SORT render_engine_sources)

if(CMAKE_SCRIPT_MODE_FILE STREQUAL CMAKE_CURRENT_LIST_FILE)
    foreach(source IN LISTS render_engine_sources renderer_sources)
        message(NOTICE "${source}")
    endforeach()
endif()
