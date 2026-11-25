cmake_minimum_required(VERSION 3.21...3.31)
project(autana_render_scene LANGUAGES C)

set(CMAKE_C_FLAGS "")
separate_arguments(scene_defines NATIVE_COMMAND "${SCENE_DEFINES}")
add_compile_options(-std=c11 -Wall -Wextra
                    -Wno-unused-parameter -Wno-unused-function -Wno-unused-variable -O1 -g
                    ${scene_defines})
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(SCENE_PROVIDER)
    add_compile_definitions("RENDER_HOST_PROVIDER=${SCENE_PROVIDER}")
endif()
# Only the file store reads the folder; on every source it would make each build folder's objects differ.
if(SCENE_ASSET_DIR)
    get_filename_component(asset_store_file "${CMAKE_CURRENT_SOURCE_DIR}/../../../main/asset/asset_store_file.c" ABSOLUTE)
    set_source_files_properties("${asset_store_file}" PROPERTIES
                                COMPILE_DEFINITIONS "ASSET_DIR_DEFAULT_PATH=\"${SCENE_ASSET_DIR}\"")
endif()
add_subdirectory(.. render_host)
add_executable(render_scene ${SCENE_SOURCES})
target_include_directories(render_scene PRIVATE ${SCENE_INCLUDES})
target_link_libraries(render_scene PRIVATE autana::${SCENE_TARGET})
# The generator expression prevents multi-config generators adding a config subfolder.
set_target_properties(render_scene PROPERTIES
    OUTPUT_NAME "${SCENE_NAME}_render"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${SCENE_OUT_DIR}>")

set(dependency_sources "$<JOIN:$<TARGET_PROPERTY:render_scene,SOURCES>,\n>\n")
get_target_property(host_libraries render_engine INTERFACE_LINK_LIBRARIES)
foreach(library IN LISTS host_libraries)
    if(TARGET ${library})
        get_target_property(sources ${library} SOURCES)
        if(sources)
            get_target_property(directory ${library} SOURCE_DIR)
            foreach(source IN LISTS sources)
                get_filename_component(source "${source}" ABSOLUTE BASE_DIR "${directory}")
                string(APPEND dependency_sources "${source}\n")
            endforeach()
        endif()
    endif()
endforeach()
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/dependency_sources.txt" CONTENT "${dependency_sources}")
