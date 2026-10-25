if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21...3.31)
    project(physics C)
endif()

# physics: state advanced by integrating forces (README.md). Header-only and
# using math, so it registers with no sources and declares math once below for
# both builds (docs/firmware/Layers.md). Built on its own, it adds the packages
# it requires itself.
set(physics_requires math)

if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS "include" REQUIRES ${physics_requires})
else()
    foreach(required IN LISTS physics_requires)
        if(NOT TARGET ${required})
            add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../${required}" "${CMAKE_BINARY_DIR}/packages/${required}")
        endif()
    endforeach()
    add_library(physics INTERFACE)
    target_include_directories(physics INTERFACE include)
    target_link_libraries(physics INTERFACE ${physics_requires})
    include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/PackageTests.cmake")
    enable_testing()
    testkit_add_package_tests()
endif()
