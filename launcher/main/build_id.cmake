set(build_id "unknown-${VARIANT}")
set(build_id_short "unknown")

execute_process(
    COMMAND git -C "${SOURCE_DIR}" rev-parse --verify HEAD
    OUTPUT_VARIABLE commit
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE commit_result
    ERROR_QUIET)

if(commit_result EQUAL 0)
    string(SUBSTRING "${commit}" 0 12 commit_short)
    execute_process(
        COMMAND git -C "${SOURCE_DIR}" status --porcelain --untracked-files=normal
        OUTPUT_VARIABLE changes
        RESULT_VARIABLE changes_result
        ERROR_QUIET)
    if(changes_result EQUAL 0 AND NOT changes STREQUAL "")
        set(build_id "${commit_short}-dirty-${VARIANT}")
    else()
        set(build_id "${commit_short}-${VARIANT}")
    endif()
    string(SUBSTRING "${commit_short}" 0 7 build_id_short)
endif()

set(header "#pragma once\n#define BUILD_ID \"${build_id}\"\n#define BUILD_ID_SHORT \"${build_id_short}\"\n")
set(header_path "${GENERATED_DIR}/build_id_generated.h")
if(EXISTS "${header_path}")
    file(READ "${header_path}" old_header)
endif()
if(NOT "${old_header}" STREQUAL "${header}")
    file(WRITE "${header_path}" "${header}")
endif()

set(id_path "${BUILD_DIR}/build_id.txt")
if(EXISTS "${id_path}")
    file(READ "${id_path}" old_id)
endif()
if(NOT "${old_id}" STREQUAL "${build_id}\n")
    file(WRITE "${id_path}" "${build_id}\n")
endif()
