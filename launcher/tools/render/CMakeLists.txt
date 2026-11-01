cmake_minimum_required(VERSION 3.21...3.31)
project(autana_render_host LANGUAGES C)

get_filename_component(launcher "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(folder_list "${launcher}/main/renderer_folders.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${folder_list}")
file(STRINGS "${folder_list}" renderer_folders REGEX "^[^#]")
set(renderer_sources "")
foreach(folder IN LISTS renderer_folders)
    string(STRIP "${folder}" folder)
    if(folder STREQUAL "" OR folder MATCHES "^#")
        continue()
    endif()
    file(GLOB_RECURSE folder_sources CONFIGURE_DEPENDS "${launcher}/main/${folder}/*.c")
    list(APPEND renderer_sources ${folder_sources})
endforeach()
file(GLOB_RECURSE drawing_sources CONFIGURE_DEPENDS
     "${launcher}/main/gfx/*.c" "${launcher}/main/draw/*.c")
list(FILTER drawing_sources EXCLUDE REGEX "_device\\.c$")
set(host_sources ${drawing_sources})
foreach(asset asset_pack asset_table asset_file asset_store asset_store_file)
    list(APPEND host_sources "${launcher}/main/asset/${asset}.c")
endforeach()
list(APPEND host_sources
     "${launcher}/main/core/tune.c" "${launcher}/main/console/console_tune.c"
     "${launcher}/main/core/job.c"
     "${CMAKE_CURRENT_LIST_DIR}/render_host.c"
     "${CMAKE_CURRENT_LIST_DIR}/render_video.c"
     "${CMAKE_CURRENT_LIST_DIR}/render_watch.c")
list(SORT host_sources)

add_library(render_host_core INTERFACE)
add_library(autana::render_host_core ALIAS render_host_core)
target_sources(render_host_core INTERFACE ${host_sources})
target_include_directories(render_host_core INTERFACE
    "${launcher}/main" "${launcher}/components/microui/include"
    "${CMAKE_CURRENT_LIST_DIR}" "${launcher}/test" "${launcher}/test/stubs")
target_compile_options(render_host_core INTERFACE -ffp-contract=off)
foreach(allocation malloc calloc realloc free)
    target_link_options(render_host_core INTERFACE "-Wl,--wrap=${allocation}")
endforeach()
if(NOT WIN32)
    target_link_libraries(render_host_core INTERFACE m)
endif()

add_compile_options(-ffp-contract=off)
file(GLOB package_lists CONFIGURE_DEPENDS "${launcher}/packages/*/CMakeLists.txt")
set(launcher_packages "")
foreach(package_list IN LISTS package_lists)
    get_filename_component(package_dir "${package_list}" DIRECTORY)
    get_filename_component(package "${package_dir}" NAME)
    if(NOT TARGET ${package})
        add_subdirectory("${package_dir}" "${CMAKE_BINARY_DIR}/packages/${package}")
    endif()
    get_directory_property(package_test_only DIRECTORY "${package_dir}" DEFINITION ${package}_test_only)
    if(NOT package_test_only)
        list(APPEND launcher_packages ${package})
    endif()
endforeach()
target_link_libraries(render_host_core INTERFACE ${launcher_packages})

# Consumers of the renderer define display_quarter_now (display/display.h).
list(SORT renderer_sources)
add_library(render_host INTERFACE)
add_library(autana::render_host ALIAS render_host)
target_sources(render_host INTERFACE ${renderer_sources})
target_link_libraries(render_host INTERFACE autana::render_host_core)

if(PROJECT_IS_TOP_LEVEL)
    add_library(render_host_check OBJECT)
    target_link_libraries(render_host_check PRIVATE autana::render_host)
endif()
