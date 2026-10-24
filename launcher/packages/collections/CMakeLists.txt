if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        project(collections C)
        enable_testing()
    endif()
endif()

# collections: containers and small algorithms over caller-owned storage
# (README.md). Header-only and depending on nothing but the C library, so it
# registers with no sources and no REQUIRES and every layer may use it.
if(ESP_PLATFORM)
    idf_component_register(INCLUDE_DIRS "include")
else()
    add_library(collections INTERFACE)
    target_include_directories(collections INTERFACE include)
endif()

if(NOT ESP_PLATFORM)
    include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/PackageTests.cmake")
    testkit_add_package_tests(collections)
endif()
