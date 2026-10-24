#include <stdbool.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "collections/strbuf.h"

static void
test_strbuf_appends_pieces_in_order(void) {
    char text[16];
    strbuf_t b = strbuf_make(text, sizeof text);
    TEST_ASSERT_TRUE(strbuf_appendf(&b, "ab"));
    TEST_ASSERT_TRUE(strbuf_appendf(&b, "%d-%s", 12, "cd"));
    TEST_ASSERT_EQUAL_STRING("ab12-cd", text);
    TEST_ASSERT_EQUAL_size_t(strlen("ab12-cd"), b.length);
    TEST_ASSERT_FALSE(b.truncated);
}

static void
test_strbuf_leaves_out_a_piece_that_does_not_fit_whole(void) {
    char text[8];
    strbuf_t b = strbuf_make(text, sizeof text);
    TEST_ASSERT_TRUE(strbuf_appendf(&b, "abcd"));
    TEST_ASSERT_FALSE(strbuf_appendf(&b, "%d", 1234));
    TEST_ASSERT_EQUAL_STRING("abcd", text);
    TEST_ASSERT_TRUE(b.truncated);
    TEST_ASSERT_TRUE(strbuf_appendf(&b, "efg"));
    TEST_ASSERT_EQUAL_STRING("abcdefg", text);
    TEST_ASSERT_FALSE(strbuf_appendf(&b, "h"));
    TEST_ASSERT_EQUAL_STRING("abcdefg", text);
}

static void
test_strbuf_continues_text_already_written(void) {
    char text[12] = "head";
    strbuf_t b = strbuf_at(text, sizeof text, strlen(text));
    TEST_ASSERT_TRUE(strbuf_appendf(&b, " tail"));
    TEST_ASSERT_EQUAL_STRING("head tail", text);
}

static void
test_strbuf_of_no_bytes_writes_nothing(void) {
    char guard = 'x';
    strbuf_t b = strbuf_make(&guard, 0);
    TEST_ASSERT_TRUE(b.truncated);
    TEST_ASSERT_FALSE(strbuf_appendf(&b, "a"));
    TEST_ASSERT_EQUAL_CHAR('x', guard);
}

void
run_strbuf_suite(void) {
    RUN_TEST(test_strbuf_appends_pieces_in_order);
    RUN_TEST(test_strbuf_leaves_out_a_piece_that_does_not_fit_whole);
    RUN_TEST(test_strbuf_continues_text_already_written);
    RUN_TEST(test_strbuf_of_no_bytes_writes_nothing);
}

SUITE_REGISTER(run_strbuf_suite);
