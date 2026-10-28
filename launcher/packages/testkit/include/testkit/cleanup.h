/* Deferred cleanups registered by test fixtures. They run in reverse order
 * after the test, even when one of its assertions failed, and each runs
 * even when another fails. A deferred failure fails the test once they have
 * run, so the rest of the test still runs first. */
#pragma once

/* Most cleanups one test can have registered at once. */
#define TEST_CLEANUP_MAX          4

/* Most bytes of deferred failure messages kept per test; more are cut. */
#define TEST_DEFERRED_FAILURE_MAX 256

/* Adds `cleanup`; one already registered is not added again. Fails the test
 * when TEST_CLEANUP_MAX are registered. */
void suite_add_test_cleanup(void (*cleanup)(void));

/* Clears the list, then adds `cleanup`. */
void suite_set_test_cleanup(void (*cleanup)(void));
void suite_clear_test_cleanup(void);

/* Fails the test after it ends, with `message` among its deferred failures.
 * suite_clear_test_cleanup() keeps them. */
void suite_fail_after_test(const char* message);

/* The deferred failures so far, comma-separated, and forgets them; NULL when
 * there are none. suite_run_test_cleanup() fails the test with them. */
const char* suite_take_deferred_failure(void);

void suite_run_test_cleanup(void);
