# physics: state advanced by integrating forces (README.md). It uses math, so it
# declares that once below for both builds; see docs/firmware/Layers.md.
#
# One file serves both builds, as math's does: ESP-IDF registers the "physics"
# component, and a plain CMake project add_subdirectory()s it and links the
# physics target. The include root is include/, so every header is spelled
# "physics/<part>/<name>.h". While it has no source it is an INTERFACE library.
set(physics_requires math)

if(CMAKE_SCRIPT_MODE_FILE)
    file(GLOB_RECURSE physics_srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
else()
    file(GLOB_RECURSE physics_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
endif()

if(ESP_PLATFORM)
    idf_component_register(SRCS ${physics_srcs} INCLUDE_DIRS "include" REQUIRES ${physics_requires})
    if(physics_srcs AND NOT CMAKE_SCRIPT_MODE_FILE)
        target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=vla -Werror=unused-macros
                               -Werror=double-promotion -Werror=float-conversion)
    endif()
elseif(physics_srcs)
    add_library(physics STATIC ${physics_srcs})
    target_include_directories(physics PUBLIC include)
    target_link_libraries(physics PUBLIC ${physics_requires})
else()
    add_library(physics INTERFACE)
    target_include_directories(physics INTERFACE include)
    target_link_libraries(physics INTERFACE ${physics_requires})
endif()
