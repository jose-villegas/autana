# math: the engine's maths library (README.md). It depends on nothing but
# libm, so it registers with no REQUIRES and everything above it may use it.
#
# One file serves both builds. ESP-IDF registers it as the "math" component
# (launcher/CMakeLists.txt adds launcher/packages to EXTRA_COMPONENT_DIRS and
# firmware says REQUIRES math); a plain CMake project such as the editor
# add_subdirectory()s it and links the math target. Either way the include
# root is include/, so every header is spelled "math/<part>/<name>.h".
#
# ESP-IDF also evaluates this file in script mode, where CONFIGURE_DEPENDS
# is illegal; that pass only reads requirements, so a plain glob is enough.
if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE math_srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
else()
    file(GLOB_RECURSE math_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
endif()

if(ESP_PLATFORM)
    idf_component_register(SRCS ${math_srcs} INCLUDE_DIRS "include")
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        # The FPU has no double; the same refusals as the main component.
        target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=vla -Werror=unused-macros
                               -Werror=double-promotion -Werror=float-conversion)
    endif()
else()
    add_library(math STATIC ${math_srcs})
    target_include_directories(math PUBLIC include)
    if(NOT WIN32)
        target_link_libraries(math PUBLIC m)
    endif()
endif()
