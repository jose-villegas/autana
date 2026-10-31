# Package.cmake: how a launcher/packages/<name>/ package builds, for both builds.
# Its CMakeLists.txt calls project() outside ESP-IDF, sets <name>_requires, then
# includes this file and calls package_add().

# Adds every package in <name>_requires the plain-CMake build does not have yet.
# A package added this way is not top level, so its own tests stay off.
function(package_add_requires name)
    foreach(required IN LISTS ${name}_requires)
        if(NOT TARGET ${required})
            add_subdirectory("${CMAKE_CURRENT_LIST_DIR}/../${required}" "${CMAKE_BINARY_DIR}/packages/${required}")
        endif()
    endforeach()
endfunction()

# Registers the calling package: its folder's name, include/ as the include root,
# src/**/*.c as its sources (none makes it header-only) and <name>_requires as
# what it uses. ESP-IDF registers it as a component; plain CMake (a
# standalone build) gets a library target of that name, plus the package's tests
# when it is the top-level project.
#
# A macro, not a function: ESP-IDF's idf_component_register sets COMPONENT_LIB in
# its caller's scope, and its script-mode pass, which only reads requirements and
# forbids CONFIGURE_DEPENDS, reads them from the CMakeLists.txt scope and returns.
macro(package_add)
    get_filename_component(package_name "${CMAKE_CURRENT_LIST_DIR}" NAME)
    if(CMAKE_SCRIPT_MODE_FILE)
        file(GLOB_RECURSE package_srcs "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
    else()
        file(GLOB_RECURSE package_srcs CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/src/*.c")
    endif()
    if(package_srcs)
        set(package_scope PUBLIC)
    else()
        set(package_scope INTERFACE)
    endif()

    if(ESP_PLATFORM)
        idf_component_register(SRCS ${package_srcs} INCLUDE_DIRS "include" REQUIRES ${${package_name}_requires})
        if(package_srcs AND NOT CMAKE_SCRIPT_MODE_FILE)
            # The FPU has no double; the same refusals as the main component.
            target_compile_options(${COMPONENT_LIB} PRIVATE -Werror=vla -Werror=unused-macros
                                   -Werror=double-promotion -Werror=float-conversion)
        endif()
    else()
        package_add_requires(${package_name})
        if(package_srcs)
            add_library(${package_name} STATIC ${package_srcs})
        else()
            add_library(${package_name} INTERFACE)
        endif()
        target_include_directories(${package_name} ${package_scope} include)
        target_link_libraries(${package_name} ${package_scope} ${${package_name}_requires})
        if(NOT WIN32)
            target_link_libraries(${package_name} ${package_scope} m)
        endif()
        enable_testing()
        testkit_add_package_tests()
    endif()
endmacro()

function(testkit_add_package_tests)
    set(package ${PROJECT_NAME})
    string(TOUPPER "${package}" prefix)
    option(${prefix}_BUILD_TESTS "Build ${package} tests" ${PROJECT_IS_TOP_LEVEL})
    if(${prefix}_BUILD_TESTS)
        if(NOT TARGET testkit_runner)
            add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../testkit" "${CMAKE_BINARY_DIR}/packages/testkit")
        endif()
        file(GLOB test_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.c")
        file(GLOB bench_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/bench/*.c")
        add_executable(${package}_tests ${test_sources} ${bench_sources})
        target_link_libraries(${package}_tests PRIVATE ${package} testkit_runner)
        set_target_properties(${package}_tests PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
        set(warnings -Wall -Wextra -Werror -Werror=vla -ffp-contract=off -Wno-unused-parameter)
        target_compile_options(${package}_tests PRIVATE ${warnings})
        add_test(NAME ${package} COMMAND ${package}_tests)
        if(IS_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bench")
            add_test(NAME ${package}_bench COMMAND ${package}_tests --on-request)
        endif()
    endif()
endfunction()
