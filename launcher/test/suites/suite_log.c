/*
 * Host suite: log, a line reaches the console whole however long it is. The
 * ESP-IDF log is replaced by a capture, so the device build, which has the
 * real one, runs none of it.
 */

#include "suites.h"

#ifndef DEVICE_BUILD

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "unity.h"
#include "util/log.h"

#define LONGEST       700
#define CAPTURE_BYTES (2 * LONGEST)

static char* captured;
static size_t captured_length;

static void
append(const char* format, va_list args) {
    const int written = vsnprintf(captured + captured_length, CAPTURE_BYTES - captured_length, format, args);
    captured_length += (size_t)written;
}

void
esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...) {
    (void)level;
    (void)tag;
    va_list args;
    va_start(args, format);
    append(format, args);
    va_end(args);
}

void
esp_log_writev(esp_log_level_t level, const char* tag, const char* format, va_list args) {
    (void)level;
    (void)tag;
    append(format, args);
}

uint32_t
esp_log_timestamp(void) {
    return 1234;
}

static void
start_capture(void) {
    captured = calloc(1, CAPTURE_BYTES);
    TEST_ASSERT_NOT_NULL(captured);
    captured_length = 0;
}

static void
end_capture(void) {
    free(captured);
    captured = NULL;
}

static void
test_a_line_longer_than_any_buffer_arrives_whole(void) {
    char* text = malloc(LONGEST + 1);
    TEST_ASSERT_NOT_NULL(text);
    memset(text, 'x', LONGEST);
    text[LONGEST - 1] = '$';
    text[LONGEST] = 0;
    start_capture();

    log_info("tag", "ms/frame %s", text);

    const char* prefix = "I (1234) tag: ms/frame ";
    TEST_ASSERT_EQUAL_UINT(strlen(prefix) + LONGEST + 1, strlen(captured));
    TEST_ASSERT_EQUAL_INT(0, strncmp(captured, prefix, strlen(prefix)));
    TEST_ASSERT_EQUAL_STRING("$\n", captured + strlen(captured) - 2);
    end_capture();
    free(text);
}

static void
test_each_level_keeps_its_letter(void) {
    start_capture();
    log_warn("t", "a");
    log_error("t", "b");
    TEST_ASSERT_EQUAL_STRING("W (1234) t: a\nE (1234) t: b\n", captured);
    end_capture();
}

#endif

void
run_log_suite(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_a_line_longer_than_any_buffer_arrives_whole);
    RUN_TEST(test_each_level_keeps_its_letter);
#endif
}

SUITE_REGISTER(run_log_suite);
