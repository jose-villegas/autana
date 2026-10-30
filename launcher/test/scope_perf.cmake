list(APPEND perf_scope_srcs
    "${CMAKE_CURRENT_LIST_DIR}/../packages/math/bench/suite_math_perf.c"
    "${CMAKE_CURRENT_LIST_DIR}/../packages/simd/bench/suite_simd_perf.c"
    "${CMAKE_CURRENT_LIST_DIR}/suites/suite_skin_light_perf.c"
    "${CMAKE_CURRENT_LIST_DIR}/suites/suite_ridge_perf.c"
    "${CMAKE_CURRENT_LIST_DIR}/suites/suite_boot_anim_perf.c"
)
