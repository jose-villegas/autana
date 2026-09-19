cmake_minimum_required(VERSION 3.24)

project(autana_editor LANGUAGES C CXX)

include(CTest)

find_package(Python3 COMPONENTS Interpreter)

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

set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.12.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

add_library(editor_runtime STATIC
    runtime/runtime.c
    ${LAUNCHER_DIR}/main/gfx/gfx.c
    ${LAUNCHER_DIR}/main/input/gesture.c
    ${LAUNCHER_DIR}/main/ui/system_navigation.c
    ${LAUNCHER_DIR}/main/ui/ui.c
    ${LAUNCHER_DIR}/main/ui/ui_build.c
    ${LAUNCHER_DIR}/main/ui/ui_control_center.c
    ${LAUNCHER_DIR}/main/ui/ui_control_center_draw.c
    ${LAUNCHER_DIR}/main/ui/ui_launcher.c
    ${LAUNCHER_DIR}/main/ui/ui_launcher_draw.c
    ${LAUNCHER_DIR}/main/ui/ui_pointer.c
    ${LAUNCHER_DIR}/main/ui/ui_scroll.c
    ${LAUNCHER_DIR}/components/microui/src/microui.c
)
target_include_directories(editor_runtime
    PUBLIC include
    PRIVATE
        runtime/host
        ${LAUNCHER_DIR}/main
        ${LAUNCHER_DIR}/components/microui/include
)
# ui_scroll.c's momentum uses expf(); only MinGW links libm unasked.
if(NOT WIN32)
    target_link_libraries(editor_runtime PUBLIC m)
endif()
target_compile_definitions(editor_runtime PRIVATE
    CONFIG_LAUNCHER_DEVELOPMENT=0
)

if(MSVC)
    target_compile_options(editor_runtime PRIVATE /W4)
else()
    # The device-only presentation statics are intentionally unused by a
    # host build. The atlas stores byte masks in char rows, so signed-char
    # hosts also warn on the intentional 0xff values.
    target_compile_options(editor_runtime PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wno-overflow
        -Wno-unused-function
        -Wno-unused-parameter
        -Wno-unused-variable
    )
endif()

add_library(editor_document STATIC
    src/layout_document.cpp
)
target_include_directories(editor_document PUBLIC src)
target_link_libraries(editor_document PUBLIC nlohmann_json::nlohmann_json)

if(MSVC)
    target_compile_options(editor_document PRIVATE /W4 /permissive-)
else()
    target_compile_options(editor_document PRIVATE -Wall -Wextra -Wpedantic)
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
        src/core/rgb565_texture.cpp
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
    if(Python3_Interpreter_FOUND)
        target_compile_definitions(autana_editor PRIVATE
            EDITOR_PYTHON_EXECUTABLE="${Python3_EXECUTABLE}"
        )
    endif()

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

    add_executable(editor_runtime_smoke tests/runtime_smoke.c)
    target_link_libraries(editor_runtime_smoke PRIVATE editor_runtime)
    add_test(NAME editor_runtime_smoke COMMAND editor_runtime_smoke)

    add_executable(editor_layout_document_test tests/layout_document_test.cpp)
    target_link_libraries(editor_layout_document_test PRIVATE editor_document GTest::gmock_main)
    target_compile_definitions(editor_layout_document_test PRIVATE
        EDITOR_TEST_LAYOUT_PATH="${LAUNCHER_DIR}/main/ui/control_center_layout.json"
    )
    gtest_discover_tests(editor_layout_document_test TEST_PREFIX "editor_document.")

    add_executable(editor_core_test tests/edit_history_test.cpp)
    target_include_directories(editor_core_test PRIVATE src)
    target_link_libraries(editor_core_test PRIVATE GTest::gtest_main)
    gtest_discover_tests(editor_core_test TEST_PREFIX "editor_core.")

    add_executable(editor_system_navigation_test tests/system_navigation_test.cpp)
    target_include_directories(editor_system_navigation_test PRIVATE ${LAUNCHER_DIR}/main)
    target_link_libraries(editor_system_navigation_test PRIVATE editor_runtime GTest::gtest_main)
    gtest_discover_tests(editor_system_navigation_test TEST_PREFIX "editor_navigation.")

    if(Python3_Interpreter_FOUND)
        if(EDITOR_BUILD_GUI)
            add_test(NAME editor_bake_check COMMAND autana_editor --check-bake)
        endif()
        add_test(
            NAME ui_layout_generator
            COMMAND ${Python3_EXECUTABLE}
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/ui_layout_generator_test.py
        )
    endif()
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
        editor_system_navigation_test
        editor_runtime_smoke
    )
    foreach(target IN LISTS EDITOR_COVERAGE_TARGETS)
        target_compile_options(${target} PRIVATE --coverage -O0 -g)
        target_link_options(${target} PRIVATE --coverage)
    endforeach()
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
            --filter launcher/main/ui/system_navigation.c
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
        DEPENDS editor_layout_document_test editor_core_test
                editor_system_navigation_test editor_runtime_smoke
        USES_TERMINAL
        COMMENT "Running tests and enforcing editor coverage gates"
    )
endif()
