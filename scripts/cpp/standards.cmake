# The compiler half of the C++ rules (docs/c-style/Cpp.md), for a project that
# builds this repository as a subdirectory:
#
#   include(<this repository>/scripts/cpp/standards.cmake)
#   autana_cpp_sanitize()     # before add_subdirectory, so the engine's C is covered too
#   add_subdirectory(<this repository> <binary dir> SYSTEM)
#   autana_cpp_checks()       # after it: flags and clang-tidy on the project's own C++
#
# AUTANA_CPP_SANITIZE and AUTANA_CPP_CLANG_TIDY switch the two optional halves on.

set(AUTANA_CPP_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# The C++ flags. The first line is the core run_tests.sh's BASE_CFLAGS shares,
# plus -Wpedantic; the rest are C++ only.
# -ffp-contract=off keeps float results equal to the engine's.
set(AUTANA_CPP_FLAGS
    -Wall -Wextra -Wpedantic -Werror -Werror=vla -ffp-contract=off
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual -Wcast-align
    -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough -Wextra-semi
    -Wdeprecated -Wzero-as-null-pointer-constant -Wsuggest-override)

option(AUTANA_CPP_SANITIZE "Build everything with UndefinedBehaviorSanitizer, and AddressSanitizer on Linux" OFF)
option(AUTANA_CPP_CLANG_TIDY "Run clang-tidy (.clang-tidy) on every C++ file of the project" OFF)

# The host tests' sanitizer flags, chosen by their own probe
# (host_sanitizer_flags in launcher/tools/build/host_make.sh) for the C compiler.
function(autana_cpp_sanitize)
    if(NOT AUTANA_CPP_SANITIZE)
        return()
    endif()
    find_program(AUTANA_CPP_SH sh REQUIRED)
    execute_process(
        COMMAND "${AUTANA_CPP_SH}" -c ". \"$1\" && host_sanitizer_flags \"$2\"" sh
                "${AUTANA_CPP_ROOT}/launcher/tools/build/host_make.sh" "${CMAKE_C_COMPILER}"
        OUTPUT_VARIABLE flags OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "${CMAKE_C_COMPILER} builds no sanitized program")
    endif()
    separate_arguments(flags UNIX_COMMAND "${flags}")
    add_compile_options(${flags})
    add_link_options(${flags})
    message(STATUS "Sanitizers: ${flags}")
endfunction()

# The flags on this directory's C++, and clang-tidy at the formatter's pinned LLVM major.
function(autana_cpp_checks)
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:${AUTANA_CPP_FLAGS}>")
    if(NOT AUTANA_CPP_CLANG_TIDY)
        return()
    endif()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${AUTANA_CPP_ROOT}/scripts/lib/pinned_tool.py" clang-tidy
                    OUTPUT_VARIABLE tidy OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
    if(NOT status EQUAL 0)
        message(FATAL_ERROR "No clang-tidy at the pinned LLVM major")
    endif()
    # pinned_tool.py prints the major, then the path.
    string(REGEX REPLACE "^[^\n]*\n" "" tidy "${tidy}")
    message(STATUS "clang-tidy: ${tidy}")
    set(CMAKE_CXX_CLANG_TIDY "${tidy}" PARENT_SCOPE)
endfunction()
