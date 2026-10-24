# Host UI editor and shared layout baker.
cmake_minimum_required(VERSION 3.24)

project(autana_editor LANGUAGES C CXX)

include(CTest)

find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(EDITOR_ENABLE_COVERAGE "Instrument editor-owned host targets for coverage" OFF)
# OFF builds and tests everything except the window, so CI needs neither SDL2
# nor Dear ImGui nor a display server's development packages.
option(EDITOR_BUILD_GUI "Build the SDL2 + Dear ImGui editor executable" ON)

# A MinGW binary otherwise loads whichever libstdc++-6.dll PATH reaches
# first, and Git for Windows ships an incompatible one.
if(MINGW)
    add_link_options(-static)
endif()

include(FetchContent)

set(LAUNCHER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../launcher")

# The firmware's packages (launcher/packages/<name>/), each a library target of
# its own name: its CMakeLists.txt builds one outside ESP-IDF.
file(GLOB package_lists CONFIGURE_DEPENDS ${LAUNCHER_DIR}/packages/*/CMakeLists.txt)
set(launcher_packages "")
foreach(package_list IN LISTS package_lists)
    get_filename_component(package_dir "${package_list}" DIRECTORY)
    get_filename_component(package "${package_dir}" NAME)
    if(NOT TARGET ${package})
        add_subdirectory("${package_dir}" "packages/${package}")
    endif()
    list(APPEND launcher_packages ${package})
endforeach()

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(editor_runtime STATIC runtime/runtime.c)
target_include_directories(editor_runtime
    PUBLIC include ${LAUNCHER_DIR}/main
    PRIVATE ${LAUNCHER_DIR}/components/microui/include
)
target_link_libraries(editor_runtime PUBLIC ${launcher_packages})

add_library(editor_document STATIC
    src/layout_document.cpp
)
target_include_directories(editor_document PUBLIC src)
target_link_libraries(editor_document PUBLIC nlohmann_json::nlohmann_json editor_runtime)

add_executable(editor_layout_bake src/layout_bake.cpp)
target_link_libraries(editor_layout_bake PRIVATE editor_document)
set_target_properties(editor_layout_bake PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}>"
)

if(MSVC)
    target_compile_options(editor_document PRIVATE /W4 /permissive-)
else()
    target_compile_options(editor_document PRIVATE -Wall -Wextra -Wpedantic -Werror=pedantic)
endif()

if(EDITOR_BUILD_GUI)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)

    FetchContent_Declare(
        SDL2
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-2.32.10
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(SDL2)

    # Docking ships only on upstream's docking branch; its release tags pin it.
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG v1.92.9b-docking
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(imgui)

    if(TARGET SDL2::SDL2-static)
        set(EDITOR_SDL_TARGET SDL2::SDL2-static)
    else()
        set(EDITOR_SDL_TARGET SDL2::SDL2)
    endif()

    add_library(editor_imgui STATIC
        ${imgui_SOURCE_DIR}/imgui.cpp
        ${imgui_SOURCE_DIR}/imgui_draw.cpp
        ${imgui_SOURCE_DIR}/imgui_tables.cpp
        ${imgui_SOURCE_DIR}/imgui_widgets.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
        ${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer2.cpp
    )
    target_include_directories(editor_imgui PUBLIC
        ${imgui_SOURCE_DIR}
        ${imgui_SOURCE_DIR}/backends
    )
    target_link_libraries(editor_imgui PUBLIC ${EDITOR_SDL_TARGET})

    add_library(editor_core STATIC
        src/core/dockspace.cpp
    )
    target_include_directories(editor_core PUBLIC src)
    target_link_libraries(editor_core PUBLIC
        editor_imgui
        ${EDITOR_SDL_TARGET}
    )

    if(MSVC)
        target_compile_options(editor_core PRIVATE /W4 /permissive-)
    else()
        target_compile_options(editor_core PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    add_executable(autana_editor
        src/main.cpp
    )
    target_include_directories(autana_editor PRIVATE include)
    target_link_libraries(autana_editor PRIVATE
        editor_document
        editor_core
        editor_imgui
        editor_runtime
        ${EDITOR_SDL_TARGET}
    )
    target_compile_definitions(autana_editor PRIVATE
        EDITOR_PROJECT_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/.."
    )

    if(MSVC)
        target_compile_options(autana_editor PRIVATE /W4 /permissive-)
    else()
        target_compile_options(autana_editor PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

if(BUILD_TESTING)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(googletest)

    include(GoogleTest)

    set(EDITOR_SAMPLE_LAYOUT "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sample_layout.json")
    set(EDITOR_SAMPLE_HEADER "${CMAKE_CURRENT_BINARY_DIR}/sample_layout_generated.h")
    add_custom_command(
        OUTPUT "${EDITOR_SAMPLE_HEADER}"
        COMMAND editor_layout_bake "${EDITOR_SAMPLE_LAYOUT}" "${EDITOR_SAMPLE_HEADER}"
        DEPENDS editor_layout_bake "${EDITOR_SAMPLE_LAYOUT}"
        VERBATIM
    )
    add_executable(editor_layout_bake_c_test tests/layout_bake_c_test.c "${EDITOR_SAMPLE_HEADER}")
    target_include_directories(editor_layout_bake_c_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" ${LAUNCHER_DIR}/main)
    target_link_libraries(editor_layout_bake_c_test PRIVATE math)
    add_test(NAME editor_layout_bake_c COMMAND editor_layout_bake_c_test)

    add_executable(editor_layout_document_test tests/layout_document_test.cpp)
    target_link_libraries(editor_layout_document_test PRIVATE editor_document GTest::gmock_main)
    target_compile_definitions(editor_layout_document_test PRIVATE
        EDITOR_TEST_LAYOUT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sample_layout.json"
        EDITOR_TEST_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/layout_document"
    )
    gtest_discover_tests(editor_layout_document_test TEST_PREFIX "editor_document.")

    add_executable(editor_core_test tests/edit_history_test.cpp)
    target_include_directories(editor_core_test PRIVATE src ${LAUNCHER_DIR}/main)
    target_link_libraries(editor_core_test PRIVATE GTest::gtest_main)
    gtest_discover_tests(editor_core_test TEST_PREFIX "editor_core.")
endif()

if(EDITOR_ENABLE_COVERAGE)
    if(NOT BUILD_TESTING)
        message(FATAL_ERROR "EDITOR_ENABLE_COVERAGE requires BUILD_TESTING=ON")
    endif()
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "EDITOR_ENABLE_COVERAGE requires GCC or Clang")
    endif()
    if(NOT Python3_Interpreter_FOUND)
        message(FATAL_ERROR "EDITOR_ENABLE_COVERAGE requires Python 3 with gcovr installed")
    endif()

    execute_process(
        COMMAND ${Python3_EXECUTABLE} -c "import gcovr"
        RESULT_VARIABLE EDITOR_GCOVR_RESULT
        OUTPUT_QUIET
        ERROR_QUIET
    )
    if(NOT EDITOR_GCOVR_RESULT EQUAL 0)
        message(FATAL_ERROR
            "Coverage requires gcovr. Install it with: ${Python3_EXECUTABLE} -m pip install gcovr==8.6"
        )
    endif()

    set(EDITOR_COVERAGE_TARGETS
        editor_document
        editor_runtime
        editor_layout_document_test
        editor_core_test
    )
    foreach(target IN LISTS EDITOR_COVERAGE_TARGETS)
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
    endforeach()
    target_link_options(editor_layout_bake PRIVATE --coverage)
    # The editor consumes instrumented static libraries but is not itself in
    # the coverage scope, so it only needs the gcov link runtime.
    if(EDITOR_BUILD_GUI)
        target_link_options(autana_editor PRIVATE --coverage)
    endif()
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        foreach(target IN LISTS EDITOR_COVERAGE_TARGETS)
            target_compile_options(${target} PRIVATE -fprofile-abs-path)
        endforeach()
    endif()

    add_custom_target(editor_coverage
        COMMAND ${CMAKE_COMMAND} -E rm -rf ${CMAKE_BINARY_DIR}/coverage
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage
        COMMAND ${CMAKE_CTEST_COMMAND} --test-dir ${CMAKE_BINARY_DIR} --output-on-failure
        COMMAND ${Python3_EXECUTABLE} -m gcovr
            --root ${CMAKE_CURRENT_SOURCE_DIR}/..
            --gcov-object-directory ${CMAKE_BINARY_DIR}
            --filter editor/src/layout_document.cpp
            --filter editor/src/core/edit_history.h
            --filter editor/runtime/runtime.c
            --exclude-throw-branches
            --exclude-unreachable-branches
            --delete
            --txt-summary
            --html-details ${CMAKE_BINARY_DIR}/coverage/index.html
            --xml ${CMAKE_BINARY_DIR}/coverage/coverage.xml
            --xml-pretty
            --fail-under-line 80
            --fail-under-branch 70
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
        DEPENDS editor_layout_bake editor_layout_document_test editor_core_test
        USES_TERMINAL
        COMMENT "Running tests and enforcing editor coverage gates"
    )
endif()
