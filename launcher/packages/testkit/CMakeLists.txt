set(testkit_test_only TRUE)
set(testkit_requires math)

if(ESP_PLATFORM)
    idf_build_get_property(testkit_arch IDF_TARGET_ARCH)
    set(testkit_idf_requires unity esp_mm esp_timer esp_app_format heap)
    if(testkit_arch STREQUAL "xtensa")
        list(APPEND testkit_idf_requires perfmon)
    endif()
    idf_component_register(SRCS "harness/bench.c" "harness/cleanup.c" "harness/registry.c"
                           INCLUDE_DIRS "include" REQUIRES ${testkit_requires} ${testkit_idf_requires})
    return()
endif()

if(NOT TARGET math)
    set(MATH_BUILD_TESTS OFF)
    add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../math" "${CMAKE_BINARY_DIR}/packages/math")
endif()

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)
add_library(testkit STATIC harness/bench.c harness/cleanup.c harness/registry.c)
target_include_directories(testkit PUBLIC include)
target_link_libraries(testkit PUBLIC unity ${testkit_requires})
add_library(testkit_runner OBJECT runner/host_main.c runner/lifecycle.c)
target_link_libraries(testkit_runner PUBLIC testkit)

set_target_properties(testkit testkit_runner unity PROPERTIES
                      C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
foreach(target testkit testkit_runner)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Werror=vla
                           -ffp-contract=off -Wno-unused-parameter)
endforeach()
