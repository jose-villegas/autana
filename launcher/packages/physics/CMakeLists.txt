# physics: state advanced by integrating forces (README.md). It uses math,
# declared once below for both builds (docs/firmware/Layers.md).
#
# ESP-IDF also evaluates this file in script mode, where CONFIGURE_DEPENDS
# is illegal; that pass only reads requirements, so a plain glob is enough.
set(physics_requires math)

if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE physics_srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
else()
    file(GLOB_RECURSE physics_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
endif()

if(ESP_PLATFORM)
    idf_component_register(SRCS ${physics_srcs} INCLUDE_DIRS "include" REQUIRES ${physics_requires})
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        # The FPU has no double; the same refusals as the main component.
        target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=vla -Werror=unused-macros
                               -Werror=double-promotion -Werror=float-conversion)
    endif()
else()
    add_library(physics STATIC ${physics_srcs})
    target_include_directories(physics PUBLIC include)
    target_link_libraries(physics PUBLIC ${physics_requires})
endif()
