#include "suites.h"

#ifndef DEVICE_BUILD
#include <stdarg.h>
#include <stdio.h>

#include "gfx/gfx.h"
#include "gfx/present/gfx_test.h"
#include "test_cleanup.h"
#include "unity.h"

#include "../app_examples/arena.c"
#include "../app_examples/hello.c"

#define CONSOLE_REPLY_BYTES 64

static char replies[CONSOLE_REPLY_BYTES];
static size_t reply_bytes;

static int
capture_printf(const char* format, ...) {
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(replies + reply_bytes, sizeof replies - reply_bytes, format, args);
    va_end(args);
    TEST_ASSERT_TRUE(written >= 0 && (size_t)written < sizeof replies - reply_bytes);
    reply_bytes += (size_t)written;
    return written;
}

#define printf capture_printf
#include "../app_examples/console.c"
#undef printf

static void
cleanup(void) {
    gfx_reset_for_test();
    app_arena_rewind(0);
    app_registry_reset_for_test();
}

static void
fixture(void) {
    suite_set_test_cleanup(cleanup);
    reply_bytes = 0;
    replies[0] = '\0';
    app_arena_rewind(0);
    app_registry_reset_for_test();
}

static void
test_hello_registers_and_draws_text_on_its_background(void) {
    fixture();
    app_yours_register();
    TEST_ASSERT_EQUAL_PTR(&app_yours, app_list());
    TEST_ASSERT_TRUE(gfx_init());
    app_yours.enter();
    app_yours.frame(0, NULL);
    gfx_color_t* pixels = gfx_framebuffer();
    TEST_ASSERT_EQUAL_HEX16(gfx_rgb(HELLO_BACKGROUND_RGB), pixels[0]);
    size_t foreground = 0;
    for (int y = HELLO_TEXT_Y; y < HELLO_TEXT_Y + DRAW_CHAR_H; y++) {
        for (int x = HELLO_TEXT_X; x < HELLO_TEXT_X + DRAW_CHAR_W; x++) {
            foreground += pixels[y * GFX_WIDTH + x] == gfx_rgb(HELLO_TEXT_RGB);
        }
    }
    TEST_ASSERT_GREATER_THAN(0, foreground);
    app_yours.exit();
}

static void
test_arena_callbacks_borrow_memory_until_the_shell_rewinds(void) {
    fixture();
    arena_enter();
    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT(BUF_BYTES, app_arena_mark());
    TEST_ASSERT_EQUAL_UINT(0, (uintptr_t)buf % _Alignof(gfx_color_t));
    arena_exit();
    TEST_ASSERT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT(BUF_BYTES, app_arena_mark());
    app_arena_rewind(0);
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES, 1));
}

static void
test_arena_exhaustion_keeps_a_null_fallback(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES, 1));
    arena_enter();
    TEST_ASSERT_NULL(buf);
    arena_exit();
}

static void
test_console_status_prints_two_lines_and_declines_other_commands(void) {
    fixture();
    TEST_ASSERT_NOT_NULL(app_example.console);
    TEST_ASSERT_EQUAL_STRING("example", app_example.console->prefix);
    TEST_ASSERT_FALSE(app_example.console->handle("unknown"));
    TEST_ASSERT_EQUAL_UINT(0, reply_bytes);
    TEST_ASSERT_TRUE(app_example.console->handle("status"));
    TEST_ASSERT_EQUAL_STRING("EXAMPLE status=ok\nEXAMPLE_END\n", replies);
}
#endif

void
suite_app_examples(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_hello_registers_and_draws_text_on_its_background);
    RUN_TEST(test_arena_callbacks_borrow_memory_until_the_shell_rewinds);
    RUN_TEST(test_arena_exhaustion_keeps_a_null_fallback);
    RUN_TEST(test_console_status_prints_two_lines_and_declines_other_commands);
#endif
}

SUITE_REGISTER(suite_app_examples);
