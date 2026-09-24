# Appended to perf_scope_srcs: every source a perf-scoped image links, not only
# the ones named _perf - a builder or fixture left out fails at link.
list(APPEND perf_scope_srcs
    "${CMAKE_CURRENT_LIST_DIR}/tests/suite_sand_perf.c"
    "${CMAKE_CURRENT_LIST_DIR}/tests/suite_sand_scenes.c"
    "${CMAKE_CURRENT_LIST_DIR}/tests/suite_sand_common.c"
)
