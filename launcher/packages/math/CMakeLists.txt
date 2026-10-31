if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21...3.31)
    project(math C)
endif()

# math: the engine's maths library (README.md). It depends on nothing but
# libm, so it requires nothing and everything above it may use it.
#
# One file serves both builds (testkit/cmake/Package.cmake). ESP-IDF registers
# it as the "math" component (launcher/CMakeLists.txt adds launcher/packages to
# EXTRA_COMPONENT_DIRS and firmware says REQUIRES math); a plain CMake project
# add_subdirectory()s it and links the math target. Either
# way the include root is include/, so every header is spelled "math/<part>/<name>.h".
include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/Package.cmake")
package_add()
