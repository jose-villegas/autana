cmake_minimum_required(VERSION 3.21)
project(stream_feed_tests LANGUAGES C)
add_subdirectory(../.. render)
add_executable(stream_feed_probe feed_probe.c)
# Both providers link into one program, as a host tool that opens scenes and streams does.
target_link_libraries(stream_feed_probe PRIVATE autana::stream_provider autana::scene_provider)
