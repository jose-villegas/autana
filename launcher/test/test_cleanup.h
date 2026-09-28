/* Deferred cleanup registered by a test fixture. */
#pragma once

void suite_set_test_cleanup(void (*cleanup)(void));
void suite_clear_test_cleanup(void);
void suite_run_test_cleanup(void);
