# Appended to perf_scope_srcs: every source a perf-scoped image links.
list(APPEND perf_scope_srcs
    "${CMAKE_CURRENT_LIST_DIR}/tests/suite_sponza_perf.c"
    "${CMAKE_CURRENT_LIST_DIR}/tests/suite_raster_scale_perf.c"
)
