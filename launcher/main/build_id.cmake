string(TIMESTAMP build_id_time "%Y%m%dT%H%M%S" UTC)
string(RANDOM LENGTH 8 ALPHABET 0123456789abcdef build_id_random)
set(build_id "${build_id_time}-${build_id_random}-${VARIANT}")

string(SUBSTRING "${build_id_time}" 9 6 build_id_short_time)
string(SUBSTRING "${build_id_random}" 0 4 build_id_short_random)
set(build_id_short "${build_id_short_time}-${build_id_short_random}")

set(header "#pragma once\n#define BUILD_ID \"${build_id}\"\n#define BUILD_ID_SHORT \"${build_id_short}\"\n")
set(header_path "${GENERATED_DIR}/build_id_generated.h")
file(WRITE "${header_path}" "${header}")

set(id_path "${BUILD_DIR}/build_id.txt")
file(WRITE "${id_path}" "${build_id}\n")
