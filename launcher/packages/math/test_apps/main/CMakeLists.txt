file(GLOB test_sources "${CMAKE_CURRENT_LIST_DIR}/../../tests/*.c"
                       "${CMAKE_CURRENT_LIST_DIR}/../../bench/*.c")
idf_component_register(SRCS ${test_sources}
    "../../../testkit/runner/idf_main.c" "../../../testkit/runner/lifecycle.c" REQUIRES math testkit
    INCLUDE_DIRS "../../tests" WHOLE_ARCHIVE)

# Match the engine compiler pass used for benchmark kernels.
target_compile_options(${COMPONENT_LIB} PRIVATE -fno-ext-dce)
