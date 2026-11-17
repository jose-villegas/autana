#include "suites.h"

#ifndef DEVICE_BUILD
#define CONFIG_LAUNCHER_DEVELOPMENT 1

#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "gfx/present/gfx_test.h"
#include "testkit/cleanup.h"
#include "unity.h"

#include "../app_examples/arena.c"
#include "../app_examples/hello.c"

#define HELLO_BACKGROUND_RGB 0x101010
#define HELLO_TEXT_RGB       0xFFFFFF
#define HELLO_TEXT_X         20
#define HELLO_TEXT_Y         20

#include "../app_examples/console.c"
#include "image/pixel/rgb565.h"

static void
cleanup(void) {
    gfx_reset_for_test();
    app_arena_rewind(0);
    app_registry_reset_for_test();
}

static void
fixture(void) {
    suite_set_test_cleanup(cleanup);
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
    TEST_ASSERT_EQUAL_HEX16(RGB565BE(HELLO_BACKGROUND_RGB), pixels[0]);
    size_t foreground = 0;
    for (int y = HELLO_TEXT_Y; y < HELLO_TEXT_Y + DRAW_CHAR_H; y++) {
        for (int x = HELLO_TEXT_X; x < HELLO_TEXT_X + DRAW_CHAR_W; x++) {
            foreground += pixels[y * GFX_WIDTH + x] == RGB565BE(HELLO_TEXT_RGB);
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
test_console_handles_status_and_declines_other_commands(void) {
    fixture();
    TEST_ASSERT_FALSE(app_example.console->handle("unknown"));
    TEST_ASSERT_TRUE(app_example.console->handle("status"));
}
#endif

void
suite_app_examples(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_hello_registers_and_draws_text_on_its_background);
    RUN_TEST(test_arena_callbacks_borrow_memory_until_the_shell_rewinds);
    RUN_TEST(test_arena_exhaustion_keeps_a_null_fallback);
    RUN_TEST(test_console_handles_status_and_declines_other_commands);
#endif
}

SUITE_REGISTER(suite_app_examples);
