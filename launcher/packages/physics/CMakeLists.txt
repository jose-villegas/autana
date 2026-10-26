if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21...3.31)
    project(physics C)
endif()

# physics: state advanced by integrating forces (README.md). It uses math,
# declared once for both builds (docs/firmware/Layers.md).
set(physics_requires math)

include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/Package.cmake")
package_add()
