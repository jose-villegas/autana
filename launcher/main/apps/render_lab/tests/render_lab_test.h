/* render_lab_test: Render Lab's own fixture beside the lab's (lab/test/lab_test.h). */
#pragma once
#ifdef DEVICE_BUILD

/* Ladder step `step`'s RUN_TEST_CASE segment, its size: "368x448". Valid
 * until the next call. */
const char* render_lab_test_step_name(int step);
#endif
