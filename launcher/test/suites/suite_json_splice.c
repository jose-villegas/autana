/*
 * Portable suite: json_splice, one more key on a JSON object already
 * written, whole or not at all.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "util/json_splice.h"

#define GUARD        '#'

/* `{"a":1}` plus `,"k":[2]` is 15 bytes; with its terminator, 16. */
#define SPLICED      "{\"a\":1,\"k\":[2]}"
#define SPLICED_SIZE sizeof SPLICED

static char json[64];

static void
fixture(const char* start) {
    memset(json, GUARD, sizeof json);
    memcpy(json, start, strlen(start) + 1);
}

static void
test_a_key_that_fits_exactly_is_spliced_and_writes_no_further(void) {
    fixture("{\"a\":1}");
    TEST_ASSERT_TRUE(json_splice_key(json, SPLICED_SIZE, "k", "[2]"));
    TEST_ASSERT_EQUAL_STRING(SPLICED, json);
    TEST_ASSERT_EQUAL_CHAR(GUARD, json[SPLICED_SIZE]);
}

static void
test_a_key_one_byte_too_long_is_dropped_whole(void) {
    fixture("{\"a\":1}");
    TEST_ASSERT_FALSE(json_splice_key(json, SPLICED_SIZE - 1, "k", "[2]"));
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", json);
    TEST_ASSERT_EQUAL_CHAR(GUARD, json[SPLICED_SIZE - 1]);
}

static void
test_text_not_ending_in_a_brace_is_left_alone(void) {
    fixture("{\"a\":1");
    TEST_ASSERT_FALSE(json_splice_key(json, sizeof json, "k", "[2]"));
    TEST_ASSERT_EQUAL_STRING("{\"a\":1", json);

    fixture("");
    TEST_ASSERT_FALSE(json_splice_key(json, sizeof json, "k", "[2]"));
    TEST_ASSERT_EQUAL_STRING("", json);
}

static void
test_two_splices_add_two_keys_in_order(void) {
    fixture("{}");
    TEST_ASSERT_TRUE(json_splice_key(json, sizeof json, "a", "1"));
    TEST_ASSERT_TRUE(json_splice_key(json, sizeof json, "b", "{\"c\":2}"));
    TEST_ASSERT_EQUAL_STRING("{\"a\":1,\"b\":{\"c\":2}}", json);
}

static void
test_splicing_onto_a_non_empty_object_keeps_its_keys(void) {
    fixture("{\"x\":0}");
    TEST_ASSERT_TRUE(json_splice_key(json, sizeof json, "app", "{}"));
    TEST_ASSERT_EQUAL_STRING("{\"x\":0,\"app\":{}}", json);
}

static void
suite_json_splice(void) {
    RUN_TEST(test_a_key_that_fits_exactly_is_spliced_and_writes_no_further);
    RUN_TEST(test_a_key_one_byte_too_long_is_dropped_whole);
    RUN_TEST(test_text_not_ending_in_a_brace_is_left_alone);
    RUN_TEST(test_two_splices_add_two_keys_in_order);
    RUN_TEST(test_splicing_onto_a_non_empty_object_keeps_its_keys);
}

SUITE_REGISTER(suite_json_splice);
