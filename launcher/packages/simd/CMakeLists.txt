if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21...3.31)
    project(simd C)
endif()

# simd: integer lane operations on plain arrays (README.md). It depends on
# nothing, so every layer may use it; the ESP32-S3 PIE kernels under src/pie/
# build only on the board, and every other build runs the C references.
include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/Package.cmake")
package_add()
