if(ESP_PLATFORM)
    idf_component_register(SRCS "harness/cleanup.c" "harness/registry.c" INCLUDE_DIRS "include" REQUIRES unity)
    return()
endif()

add_library(unity STATIC unity/unity.c)
target_include_directories(unity PUBLIC unity)
add_library(testkit STATIC harness/cleanup.c harness/registry.c)
target_include_directories(testkit PUBLIC include)
target_link_libraries(testkit PUBLIC unity)
add_library(testkit_runner OBJECT runner/host_main.c)
target_link_libraries(testkit_runner PUBLIC testkit)

set_target_properties(testkit testkit_runner unity PROPERTIES
                      C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
foreach(target testkit testkit_runner)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -Werror=vla
                           -ffp-contract=off -Wno-unused-parameter)
endforeach()
