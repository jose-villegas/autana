#include <stdlib.h>

#include "audio/audio_out.h"
#include "sound/console/console_mic_parse.h"
#include "suites.h"
#include "unity.h"

static void
test_record_framing(void) {
    char line[128];
    TEST_ASSERT_TRUE(console_mic_begin(line, sizeof line, AUDIO_OUT_SAMPLE_RATE_HZ, 441));
    TEST_ASSERT_EQUAL_STRING("MIC_BEGIN {\"rate\":22050,\"samples\":441}", line);
    TEST_ASSERT_FALSE(console_mic_begin(line, 1, AUDIO_OUT_SAMPLE_RATE_HZ, 441));
}

static void
test_record_arguments(void) {
    console_mic_args_t* const parsed = malloc(sizeof *parsed);
    const size_t limit_size = MIC_RECORD_ARGS_MAX + 1u;
    char* const limit = malloc(limit_size);
    TEST_ASSERT_NOT_NULL(parsed);
    TEST_ASSERT_NOT_NULL(limit);

    static const struct {
        const char* command;
        bool valid;
    } cases[] = {
        {"record 3000 500 at -500 tone 1000 3500 100", true},
        {"record 3000 500 at -500 tone 1000 3501 100", false},
        {"record 3000 500 at 1 tone 1000 3500 100", false},
        {"record 3000 500 at -501 0x32 40", false},
        {"record 3000 500 at -500 0x32 40", true},
        {"record 4000 500 at 400 tone 40 80 100 at 750 tone 202 60 60", true},
        {"record 1000 at 0 0x32 40 at 900 0x32 0", true},
        {"record 1000 at 1000 0x32 0", false},
    };

    for (unsigned i = 0; i < sizeof cases / sizeof *cases; i++) {
        TEST_ASSERT_EQUAL(cases[i].valid, console_mic_parse(cases[i].command, parsed));
    }
    TEST_ASSERT_EQUAL_UINT32(2, parsed->entry_count);
    TEST_ASSERT_EQUAL_UINT32(900, parsed->entries[1].ms);
    TEST_ASSERT_EQUAL_HEX8(0x32, parsed->entries[0].write.reg);
    TEST_ASSERT_EQUAL_HEX8(40, parsed->entries[0].write.value);
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1001 0x32 0", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1 256 0", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1 0 256", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record 1000 at 1 0 0 at 2 0 0 at 3 0 0 at 4 0 0 at 5 0 0", parsed));
    TEST_ASSERT_TRUE(console_mic_parse("record 1000 at 100 tone 300 80 100 at 200 0x32 40", parsed));
    TEST_ASSERT_EQUAL_UINT32(CONSOLE_MIC_TONE, parsed->entries[0].kind);
    TEST_ASSERT_EQUAL_UINT32(300, parsed->entries[0].tone.hz);
    TEST_ASSERT_EQUAL_UINT32(80, parsed->entries[0].tone.duration_ms);
    TEST_ASSERT_EQUAL_UINT32(100, parsed->entries[0].tone.gain);
    TEST_ASSERT_EQUAL_UINT32(CONSOLE_MIC_WRITE, parsed->entries[1].kind);
    const char* invalid[] = {"record 1000 at 1 tone 0 80 100",     "record 1000 at 1 tone 300 0 100",
                             "record 1000 at 1 tone 300 80 101",   "record 1000 at 999 tone 300 80 100",
                             "record 1000 at 1 tone 11025 80 100", "record 1000 at 1 tone 300 80",
                             "record 1000 at 1 tone -1 80 100",    "record 1000 at 1 tones 300 80 100"};
    for (unsigned i = 0; i < sizeof invalid / sizeof *invalid; i++) {
        TEST_ASSERT_FALSE(console_mic_parse(invalid[i], parsed));
    }
    size_t used = (size_t)snprintf(limit, limit_size, "record 4000");
    for (unsigned i = 0; i < MIC_RECORD_TONES_MAX; i++) {
        used += (size_t)snprintf(limit + used, limit_size - used, " at %u tone 40 80 100", i * 100u);
    }
    for (unsigned i = 0; i < MIC_RECORD_WRITES_MAX; i++) {
        used += (size_t)snprintf(limit + used, limit_size - used, " at %u 0x32 40", i * 100u);
    }
    TEST_ASSERT_TRUE(console_mic_parse(limit, parsed));
    TEST_ASSERT_EQUAL_UINT32(MIC_RECORD_ENTRIES_MAX, parsed->entry_count);
    (void)snprintf(limit + used, limit_size - used, " at 0 tone 40 80 100");
    TEST_ASSERT_FALSE(console_mic_parse(limit, parsed));
    TEST_ASSERT_TRUE(console_mic_parse("record 1000 at 0 050 040", parsed));
    TEST_ASSERT_EQUAL_UINT8(50, parsed->entries[0].write.reg);
    TEST_ASSERT_EQUAL_UINT8(40, parsed->entries[0].write.value);
    TEST_ASSERT_TRUE(console_mic_parse("record 1000", parsed));
    TEST_ASSERT_EQUAL_UINT32(AUDIO_LOOPBACK_SETTLE_MS, parsed->settle_ms);
    TEST_ASSERT_TRUE(console_mic_parse("record 10000 0", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record 10001", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record 0", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record -1", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record 100 0 extra", parsed));
    TEST_ASSERT_FALSE(console_mic_parse("record 100 9999999999999999999999", parsed));
    free(limit);
    free(parsed);
}

static unsigned applied_writes;

static bool
apply_write(const console_mic_entry_t* entry) {
    applied_writes++;
    return entry->kind == CONSOLE_MIC_WRITE;
}

static void
test_record_settle_schedule(void) {
    console_mic_args_t args = {
        .entry_count = 2, .entries = {{.ms = -500, .kind = CONSOLE_MIC_WRITE}, {.ms = 0, .kind = CONSOLE_MIC_WRITE}}};
    unsigned applied = 0;
    applied_writes = 0;
    TEST_ASSERT_TRUE(console_mic_apply_entries(&args, -(int32_t)audio_out_samples(500), -1, &applied, apply_write));
    TEST_ASSERT_EQUAL_UINT32(1, applied_writes);
    TEST_ASSERT_EQUAL_UINT32(1, applied);
    TEST_ASSERT_EQUAL_INT32(-1, args.entries[0].block);
    TEST_ASSERT_TRUE(console_mic_apply_entries(&args, 0, 0, &applied, apply_write));
    TEST_ASSERT_EQUAL_UINT32(2, applied_writes);
    TEST_ASSERT_EQUAL_UINT32(3, applied);
    TEST_ASSERT_EQUAL_INT32(0, args.entries[1].block);
}

void
suite_mic_record(void) {
    RUN_TEST(test_record_framing);
    RUN_TEST(test_record_arguments);
    RUN_TEST(test_record_settle_schedule);
}
SUITE_REGISTER(suite_mic_record)
