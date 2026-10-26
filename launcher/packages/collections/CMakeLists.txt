if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21...3.31)
    project(collections C)
endif()

# collections: containers and small algorithms over caller-owned storage
# (README.md). Header-only and depending on nothing but the C library, so it
# requires nothing and every layer may use it.
include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/Package.cmake")
package_add()
