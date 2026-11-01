cmake_minimum_required(VERSION 3.21...3.31)
project(autana_render_host LANGUAGES C)

include("${CMAKE_CURRENT_LIST_DIR}/engine_sources.cmake")

# The engine with no main() and no hooks: what a host build of the firmware
# drawing code links.
add_library(render_engine INTERFACE)
add_library(autana::render_engine ALIAS render_engine)
target_sources(render_engine INTERFACE ${render_engine_sources})
target_include_directories(render_engine INTERFACE
    "${launcher}/main" "${launcher}/components/microui/include"
    "${launcher}/test" "${launcher}/test/stubs")
target_compile_options(render_engine INTERFACE -ffp-contract=off)
if(NOT WIN32)
    target_link_libraries(render_engine INTERFACE m)
endif()

add_library(render_host_core INTERFACE)
add_library(autana::render_host_core ALIAS render_host_core)
target_sources(render_host_core INTERFACE
    "${CMAKE_CURRENT_LIST_DIR}/render_host.c"
    "${CMAKE_CURRENT_LIST_DIR}/render_video.c"
    "${CMAKE_CURRENT_LIST_DIR}/render_watch.c")
target_include_directories(render_host_core INTERFACE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(render_host_core INTERFACE autana::render_engine)
foreach(allocation malloc calloc realloc free)
    target_link_options(render_host_core INTERFACE "-Wl,--wrap=${allocation}")
endforeach()

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
target_link_libraries(render_engine INTERFACE ${launcher_packages})

# Consumers of the renderer define display_quarter_now (display/display.h).
add_library(render_host INTERFACE)
add_library(autana::render_host ALIAS render_host)
target_sources(render_host INTERFACE ${renderer_sources})
target_link_libraries(render_host INTERFACE autana::render_host_core)

if(PROJECT_IS_TOP_LEVEL)
    add_library(render_host_check OBJECT)
    target_link_libraries(render_host_check PRIVATE autana::render_host)
endif()
