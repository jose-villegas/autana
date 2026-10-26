if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21...3.31)
    project(physics C)
endif()

# physics: state advanced by integrating forces (README.md). It uses math,
# declared once below for both builds (docs/firmware/Layers.md). Built on its
# own, it adds the packages it requires itself.
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
    foreach(required IN LISTS physics_requires)
        if(NOT TARGET ${required})
            add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../${required}" "${CMAKE_BINARY_DIR}/packages/${required}")
        endif()
    endforeach()
    add_library(physics STATIC ${physics_srcs})
    target_include_directories(physics PUBLIC include)
    target_link_libraries(physics PUBLIC ${physics_requires})
    include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/PackageTests.cmake")
    enable_testing()
    testkit_add_package_tests()
endif()
