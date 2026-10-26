cmake_minimum_required(VERSION 3.16)
# launcher/packages/ holds first-party libraries shipped as components of
# their own; components/ holds vendored code.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/packages")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(launcher)

# The diagnostics image keeps every object's stack frames (.su) and call graph
# (.ci) beside it: tools/quality/stack_chain_gate.py reads them, and they
# answer who calls a function, macros and inlined calls included.
if(CONFIG_LAUNCHER_SELFTEST)
    idf_build_get_property(components BUILD_COMPONENTS)
    foreach(component ${components})
        idf_component_get_property(library ${component} COMPONENT_LIB)
        get_target_property(type ${library} TYPE)
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            target_compile_options(${library} PRIVATE
                "$<$<COMPILE_LANGUAGE:C,CXX>:-fstack-usage;-fcallgraph-info=su>")
        endif()
    endforeach()
endif()

# A seeded build (main/CMakeLists.txt) fails at the link unless every layout
# pad leads its object's code and rodata, so the seed moves that code.
if(LAUNCHER_LAYOUT_SEED)
    idf_build_get_property(layout_python PYTHON)
    add_custom_command(TARGET ${CMAKE_PROJECT_NAME}.elf POST_BUILD
        COMMAND ${layout_python} "${CMAKE_CURRENT_LIST_DIR}/tools/build/layout_pad.py"
                --check "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map"
        VERBATIM)
endif()
