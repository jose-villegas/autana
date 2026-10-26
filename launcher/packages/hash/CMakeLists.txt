if(NOT ESP_PLATFORM)
    cmake_minimum_required(VERSION 3.21...3.31)
    project(hash C)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../testkit/cmake/Package.cmake")
package_add()
