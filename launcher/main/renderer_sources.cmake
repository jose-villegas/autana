# The renderer's sources: every .c below these folders of main/, so a file
# added to one is built everywhere the renderer is. Included by
# CMakeLists.txt and, for host builds, tools/render/engine_sources.cmake.
# Sets renderer_sources and defines launcher_glob.

# file(GLOB_RECURSE), re-run at build time except in script mode (cmake -P).
function(launcher_glob output)
    if(CMAKE_SCRIPT_MODE_FILE)
        file(GLOB_RECURSE files ${ARGN})
    else()
        file(GLOB_RECURSE files CONFIGURE_DEPENDS ${ARGN})
    endif()
    set(${output} ${files} PARENT_SCOPE)
endfunction()

set(renderer_sources "")
foreach(renderer_folder render scene anim)
    launcher_glob(renderer_folder_sources "${CMAKE_CURRENT_LIST_DIR}/${renderer_folder}/*.c")
    list(APPEND renderer_sources ${renderer_folder_sources})
endforeach()
list(SORT renderer_sources)
