#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "suites.h"
#include "unity.h"

#include "microui.h"

static int
text_width(mu_Font font, const char* str, int len) {
    (void)font;
    return (len < 0 ? (int)strlen(str) : len) * 8;
}

static int
text_height(mu_Font font) {
    (void)font;
    return 8;
}

static int
build_commands(mu_Context* ctx, int garbage) {
    memset(ctx, 0, sizeof *ctx);
    mu_init(ctx);
    ctx->text_width = text_width;
    ctx->text_height = text_height;
    memset(ctx->command_list.items, garbage, sizeof ctx->command_list.items);
    mu_begin(ctx);
    if (mu_begin_window(ctx, "window", mu_rect(0, 0, 160, 120)) != 0) {
        mu_label(ctx, "odd text");
        mu_button(ctx, "button");
        mu_end_window(ctx);
    }
    mu_end(ctx);
    return ctx->command_list.idx;
}

static uint64_t
command_hash(const mu_Context* ctx) {
    uint64_t hash = 1469598103934665603ull;
    for (int i = 0; i < ctx->command_list.idx; i++) {
        hash ^= (unsigned char)ctx->command_list.items[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

static void
test_commands_remain_aligned_after_text(void) {
    mu_Context* ctx = malloc(sizeof *ctx);
    TEST_ASSERT_NOT_NULL(ctx);
    build_commands(ctx, 0xa5);

    int count = 0;
    for (int offset = 0; offset < ctx->command_list.idx;) {
        TEST_ASSERT_EQUAL_UINT32(0, (uintptr_t)(ctx->command_list.items + offset) % _Alignof(mu_Command));
        mu_Command* cmd = (mu_Command*)(ctx->command_list.items + offset);
        TEST_ASSERT_TRUE(cmd->base.size > 0);
        offset += cmd->base.size;
        count++;
    }
    TEST_ASSERT_TRUE(count > 3);
    free(ctx);
}

static void
test_command_hash_ignores_initial_buffer_garbage(void) {
    mu_Context* ctx = malloc(sizeof *ctx);
    TEST_ASSERT_NOT_NULL(ctx);
    const int first_size = build_commands(ctx, 0xa5);
    const uint64_t first_hash = command_hash(ctx);
    const int second_size = build_commands(ctx, 0x5a);
    TEST_ASSERT_EQUAL_INT(first_size, second_size);
    TEST_ASSERT_EQUAL_UINT64(first_hash, command_hash(ctx));
    free(ctx);
}

void
run_microui_commands_suite(void) {
    RUN_TEST(test_commands_remain_aligned_after_text);
    RUN_TEST(test_command_hash_ignores_initial_buffer_garbage);
}

SUITE_REGISTER(run_microui_commands_suite);
