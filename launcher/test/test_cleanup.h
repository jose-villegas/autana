/* Deferred cleanups registered by test fixtures. They run in reverse order
 * after the test, even when one of its assertions failed. */
#pragma once

/* Most cleanups one test can have registered at once. */
#define TEST_CLEANUP_MAX 4

/* Adds `cleanup`; one already registered is not added again. Fails the test
 * when TEST_CLEANUP_MAX are registered. */
void suite_add_test_cleanup(void (*cleanup)(void));

/* Clears the list, then adds `cleanup`. */
void suite_set_test_cleanup(void (*cleanup)(void));
void suite_clear_test_cleanup(void);
void suite_run_test_cleanup(void);
