/*
 * Portable suite: frame_watch, a call site that repeats across frames,
 * told apart from one that fires on the frame something happened.
 */

#include <string.h>

#include "suites.h"
#include "unity.h"

#include "util/frame_watch.h"

#ifdef DEVICE_BUILD
#include <stdlib.h>

#include "esp_log.h"
#include "gfx/gfx.h"
#endif

#define SITE_A ((uintptr_t)0x42001000u)
#define SITE_B ((uintptr_t)0x42002000u)

static frame_watch_t watch;

static void
fixture(void) {
    frame_watch_reset(&watch);
}

static void
pass_warmup(void) {
    for (int i = 0; i < FRAME_WATCH_WARMUP; i++) {
        frame_watch_close_frame(&watch);
    }
}

/* Runs `frames` frames, noting SITE_A on every `every`-th one. Returns how
 * many sites became repeating over them. */
static int
note_every(int every, int frames) {
    int became = 0;
    for (int i = 0; i < frames; i++) {
        if (i % every == 0) {
            frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A);
        }
        became += frame_watch_close_frame(&watch);
    }
    return became;
}

static frame_watch_site_t*
site_of(uintptr_t site) {
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        if (watch.sites[i].seen != 0 && watch.sites[i].site == site) {
            return &watch.sites[i];
        }
    }
    return NULL;
}

static void
test_a_site_in_every_frame_becomes_repeating_at_the_repeat_count(void) {
    fixture();
    pass_warmup();
    TEST_ASSERT_EQUAL_INT(0, note_every(1, FRAME_WATCH_REPEATS - 1));
    TEST_ASSERT_EQUAL_INT(1, note_every(1, 1));
    TEST_ASSERT_EQUAL_INT(1, watch.repeating);
    TEST_ASSERT_EQUAL_INT(1, watch.ever_repeating);
}

static void
test_a_site_on_one_frame_never_repeats(void) {
    fixture();
    pass_warmup();
    note_every(1, 1);
    for (int i = 0; i < 200; i++) {
        frame_watch_close_frame(&watch);
    }
    TEST_ASSERT_EQUAL_INT(0, watch.ever_repeating);
}

static void
test_a_site_every_other_frame_is_repeating(void) {
    fixture();
    pass_warmup();
    TEST_ASSERT_EQUAL_INT(1, note_every(2, FRAME_WATCH_WINDOW));
}

static void
test_a_site_every_third_frame_is_not(void) {
    fixture();
    pass_warmup();
    TEST_ASSERT_EQUAL_INT(0, note_every(3, 10 * FRAME_WATCH_WINDOW));
}

/* A report every second and a half, at the board's fastest frame rate. */
static void
test_a_periodic_report_is_not_repeating(void) {
    fixture();
    pass_warmup();
    TEST_ASSERT_EQUAL_INT(0, note_every(90, 1000));
}

static void
test_warmup_frames_are_counted_but_not_judged(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(0, note_every(1, FRAME_WATCH_WARMUP));
    TEST_ASSERT_EQUAL_UINT32(1, watch.last[FRAME_WATCH_ALLOC]);
    TEST_ASSERT_EQUAL_UINT32(0, watch.frames);
    TEST_ASSERT_NULL(site_of(SITE_A));
    TEST_ASSERT_EQUAL_INT(1, note_every(1, FRAME_WATCH_REPEATS));
}

static void
test_a_site_that_stops_stops_repeating_and_frees_its_slot(void) {
    fixture();
    pass_warmup();
    note_every(1, FRAME_WATCH_WINDOW);
    TEST_ASSERT_EQUAL_INT(1, watch.repeating);

    for (int i = 0; i < FRAME_WATCH_WINDOW; i++) {
        frame_watch_close_frame(&watch);
    }
    TEST_ASSERT_EQUAL_INT(0, watch.repeating);
    TEST_ASSERT_NULL(site_of(SITE_A));
    TEST_ASSERT_EQUAL_INT(1, watch.ever_repeating);
}

static void
test_an_alloc_and_a_free_at_one_address_are_two_sites(void) {
    fixture();
    pass_warmup();
    frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A);
    frame_watch_note(&watch, FRAME_WATCH_FREE, SITE_A);
    int used = 0;
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        used += watch.sites[i].seen != 0;
    }
    TEST_ASSERT_EQUAL_INT(2, used);
}

static void
test_a_full_table_drops_the_site_but_still_counts_the_event(void) {
    fixture();
    pass_warmup();
    for (int i = 0; i <= FRAME_WATCH_SITES; i++) {
        frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A + (uintptr_t)i * 4u);
    }
    frame_watch_close_frame(&watch);
    TEST_ASSERT_EQUAL_UINT32(1, watch.dropped);
    TEST_ASSERT_EQUAL_UINT32(FRAME_WATCH_SITES + 1, watch.last[FRAME_WATCH_ALLOC]);
}

static void
test_the_last_frame_counts_each_kind(void) {
    fixture();
    frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A);
    frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A);
    frame_watch_note(&watch, FRAME_WATCH_FREE, SITE_A);
    frame_watch_note(&watch, FRAME_WATCH_CONSOLE, SITE_B);
    frame_watch_close_frame(&watch);
    TEST_ASSERT_EQUAL_UINT32(2, watch.last[FRAME_WATCH_ALLOC]);
    TEST_ASSERT_EQUAL_UINT32(1, watch.last[FRAME_WATCH_FREE]);
    TEST_ASSERT_EQUAL_UINT32(1, watch.last[FRAME_WATCH_CONSOLE]);

    frame_watch_close_frame(&watch);
    TEST_ASSERT_EQUAL_UINT32(0, watch.last[FRAME_WATCH_ALLOC]);
}

static void
test_a_repeating_site_is_due_once_per_interval(void) {
    fixture();
    pass_warmup();
    note_every(1, FRAME_WATCH_REPEATS);
    frame_watch_site_t* s = site_of(SITE_A);
    TEST_ASSERT_NOT_NULL(s);

    TEST_ASSERT_TRUE(frame_watch_take_due(s, 1000));
    TEST_ASSERT_FALSE(frame_watch_take_due(s, 1000 + FRAME_WATCH_REPORT_INTERVAL_US - 1));
    TEST_ASSERT_TRUE(frame_watch_take_due(s, 1000 + FRAME_WATCH_REPORT_INTERVAL_US));
}

static void
test_a_site_not_repeating_is_never_due(void) {
    fixture();
    pass_warmup();
    note_every(1, 1);
    frame_watch_site_t* s = site_of(SITE_A);
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_FALSE(frame_watch_take_due(s, 0));
}

static void
test_json_holds_the_counts_and_the_repeating_sites(void) {
    fixture();
    pass_warmup();
    for (int i = 0; i < FRAME_WATCH_REPEATS; i++) {
        frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A);
        frame_watch_note(&watch, FRAME_WATCH_CONSOLE, SITE_B);
        frame_watch_close_frame(&watch);
    }
    char json[FRAME_WATCH_JSON_MAX];
    frame_watch_format_json(&watch, json, sizeof json);
    TEST_ASSERT_EQUAL_STRING("{\"frames\":8,\"allocs\":1,\"frees\":0,\"console\":1,\"repeating\":2,\"dropped\":0,"
                             "\"sites\":[{\"kind\":\"alloc\",\"site\":\"0x42001000\"},"
                             "{\"kind\":\"console\",\"site\":\"0x42002000\"}]}",
                             json);
}

static int
json_length_with_one_site(void) {
    fixture();
    pass_warmup();
    note_every(1, FRAME_WATCH_REPEATS);
    char json[FRAME_WATCH_JSON_MAX];
    return frame_watch_format_json(&watch, json, sizeof json);
}

/* A guard byte past `size` must survive every write. */
static void
test_json_that_fits_exactly_is_written_whole_and_no_further(void) {
    const int whole = json_length_with_one_site();
    char json[FRAME_WATCH_JSON_MAX + 1];
    memset(json, '#', sizeof json);
    TEST_ASSERT_EQUAL_INT(whole, frame_watch_format_json(&watch, json, (size_t)whole + 1));
    TEST_ASSERT_NOT_NULL(strstr(json, "0x42001000"));
    TEST_ASSERT_EQUAL_CHAR('#', json[whole + 1]);
}

static void
test_json_one_byte_short_for_a_site_leaves_it_out_whole(void) {
    const int whole = json_length_with_one_site();
    char json[FRAME_WATCH_JSON_MAX + 1];
    memset(json, '#', sizeof json);
    const int length = frame_watch_format_json(&watch, json, (size_t)whole);
    TEST_ASSERT_EQUAL_INT((int)strlen(json), length);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"sites\":[]}"));
    TEST_ASSERT_EQUAL_CHAR('#', json[whole]);
}

static void
test_json_with_no_room_for_the_counts_is_empty(void) {
    fixture();
    char json[FRAME_WATCH_JSON_MAX];
    memset(json, '#', sizeof json);
    /* Volatile, or the compiler warns about the truncation it can see. */
    volatile size_t room = 8;
    TEST_ASSERT_EQUAL_INT(0, frame_watch_format_json(&watch, json, room));
    TEST_ASSERT_EQUAL_STRING("", json);
    TEST_ASSERT_EQUAL_CHAR('#', json[8]);
}

/* The window is FRAME_WATCH_WINDOW frames wide: REPEATS sightings spread
 * across exactly that many repeat, one frame wider do not. */
static int
repeats_when_spread_over(int frames) {
    fixture();
    pass_warmup();
    int became = 0;
    for (int i = 0; i < frames; i++) {
        const bool first_or_last_few = i == 0 || i >= frames - (FRAME_WATCH_REPEATS - 1);
        if (first_or_last_few) {
            frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A);
        }
        became += frame_watch_close_frame(&watch);
    }
    return became;
}

static void
test_repeats_spread_across_the_whole_window_count(void) {
    TEST_ASSERT_EQUAL_INT(1, repeats_when_spread_over(FRAME_WATCH_WINDOW));
}

static void
test_repeats_spread_one_frame_wider_than_the_window_do_not(void) {
    TEST_ASSERT_EQUAL_INT(0, repeats_when_spread_over(FRAME_WATCH_WINDOW + 1));
}

/* Three sites taking turns are each below the repeat count, together every
 * frame: they must stay three sites. */
static void
test_sites_taking_turns_are_not_merged(void) {
    fixture();
    pass_warmup();
    int became = 0;
    for (int i = 0; i < 4 * FRAME_WATCH_WINDOW; i++) {
        frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A + (uintptr_t)(i % 3) * 4u);
        became += frame_watch_close_frame(&watch);
    }
    TEST_ASSERT_EQUAL_INT(0, became);
}

/* SITE_B leaves the first slot free while SITE_A still holds the second:
 * noting SITE_A again must find its own slot, not claim the free one. */
static void
test_a_site_keeps_one_slot_when_an_earlier_slot_frees_up(void) {
    fixture();
    pass_warmup();
    frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_B);
    for (int i = 0; i < FRAME_WATCH_WINDOW + 2; i++) {
        frame_watch_note(&watch, FRAME_WATCH_ALLOC, SITE_A);
        frame_watch_close_frame(&watch);
    }
    int used = 0;
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        used += watch.sites[i].seen != 0;
    }
    TEST_ASSERT_EQUAL_INT(1, used);
    TEST_ASSERT_EQUAL_INT(1, watch.repeating);
}

static void
test_settling_forgets_a_site_and_restarts_the_warmup(void) {
    fixture();
    pass_warmup();
    note_every(1, FRAME_WATCH_REPEATS - 1);
    frame_watch_settle(&watch);

    TEST_ASSERT_EQUAL_INT(0, note_every(1, FRAME_WATCH_WARMUP));
    TEST_ASSERT_EQUAL_INT(0, watch.repeating);
    TEST_ASSERT_EQUAL_INT(0, note_every(1, FRAME_WATCH_REPEATS - 1));
    TEST_ASSERT_EQUAL_INT(1, note_every(1, 1));
}

static void
test_the_verdict_counts_what_became_repeating_and_what_was_dropped(void) {
    fixture();
    pass_warmup();
    note_every(1, FRAME_WATCH_REPEATS);
    for (int i = 0; i <= FRAME_WATCH_SITES; i++) {
        frame_watch_note(&watch, FRAME_WATCH_FREE, SITE_B + (uintptr_t)i * 4u);
    }
    frame_watch_close_frame(&watch);
    const frame_watch_verdict_t verdict = frame_watch_verdict(&watch);
    TEST_ASSERT_EQUAL_INT(1, verdict.repeating);
    TEST_ASSERT_TRUE(verdict.dropped > 0);
    TEST_ASSERT_EQUAL_UINT32(FRAME_WATCH_REPEATS + 1, verdict.frames);
    TEST_ASSERT_FALSE(frame_watch_verdict_clean(verdict));
}

#ifdef DEVICE_BUILD
/* Long enough for the warm-up and then a whole window. */
#define WATCHED_PRESENTS (FRAME_WATCH_WARMUP + FRAME_WATCH_WINDOW)

static void
allocate_and_free(void) {
    volatile char* block = malloc(32);
    if (block != NULL) {
        block[0] = 1;
    }
    free((void*)block);
}

/* Three call sites of their own, so the heap hook must tell them apart. */
static __attribute__((noinline)) void
allocate_here_0(void) {
    free(malloc(24));
}

static __attribute__((noinline)) void
allocate_here_1(void) {
    free(malloc(24));
}

static __attribute__((noinline)) void
allocate_here_2(void) {
    free(malloc(24));
}

/* The RUN_TEST wrapper already watches this test; its verdict is taken here
 * and the watch started again, so the wrapper's own finds nothing. */
static int
sites_caught_so_far(void) {
    const frame_watch_verdict_t verdict = frame_watch_test_end();
    frame_watch_test_begin();
    TEST_ASSERT_EQUAL_UINT32(0, verdict.dropped);
    return verdict.repeating;
}

static void
test_on_the_board_an_allocation_every_present_is_caught(void) {
    for (int i = 0; i < WATCHED_PRESENTS; i++) {
        allocate_and_free();
        gfx_present();
    }
    TEST_ASSERT_EQUAL_INT(2, sites_caught_so_far());
}

static void
test_on_the_board_a_log_line_every_present_is_caught(void) {
    for (int i = 0; i < WATCHED_PRESENTS; i++) {
        ESP_LOGI("frame_watch_test", "present %d", i);
        gfx_present();
    }
    TEST_ASSERT_EQUAL_INT(1, sites_caught_so_far());
}

static void
test_on_the_board_one_allocation_among_many_presents_is_not(void) {
    for (int i = 0; i < WATCHED_PRESENTS; i++) {
        if (i == WATCHED_PRESENTS - FRAME_WATCH_WINDOW / 2) {
            allocate_and_free();
        }
        gfx_present();
    }
    TEST_ASSERT_EQUAL_INT(0, sites_caught_so_far());
}

static void
test_on_the_board_three_allocation_sites_taking_turns_are_not_merged(void) {
    for (int i = 0; i < WATCHED_PRESENTS + FRAME_WATCH_WINDOW; i++) {
        switch (i % 3) {
            case 0: allocate_here_0(); break;
            case 1: allocate_here_1(); break;
            default: allocate_here_2(); break;
        }
        gfx_present();
    }
    TEST_ASSERT_EQUAL_INT(0, sites_caught_so_far());
}

static void
test_on_the_board_three_log_lines_taking_turns_are_not_merged(void) {
    for (int i = 0; i < WATCHED_PRESENTS + FRAME_WATCH_WINDOW; i++) {
        switch (i % 3) {
            case 0: ESP_LOGI("frame_watch_test", "first %d", i); break;
            case 1: ESP_LOGI("frame_watch_test", "second %d", i); break;
            default: ESP_LOGI("frame_watch_test", "third %d", i); break;
        }
        gfx_present();
    }
    TEST_ASSERT_EQUAL_INT(0, sites_caught_so_far());
}
#endif

static void
suite_frame_watch(void) {
    RUN_TEST(test_a_site_in_every_frame_becomes_repeating_at_the_repeat_count);
    RUN_TEST(test_a_site_on_one_frame_never_repeats);
    RUN_TEST(test_a_site_every_other_frame_is_repeating);
    RUN_TEST(test_a_site_every_third_frame_is_not);
    RUN_TEST(test_a_periodic_report_is_not_repeating);
    RUN_TEST(test_warmup_frames_are_counted_but_not_judged);
    RUN_TEST(test_a_site_that_stops_stops_repeating_and_frees_its_slot);
    RUN_TEST(test_an_alloc_and_a_free_at_one_address_are_two_sites);
    RUN_TEST(test_a_full_table_drops_the_site_but_still_counts_the_event);
    RUN_TEST(test_the_last_frame_counts_each_kind);
    RUN_TEST(test_a_repeating_site_is_due_once_per_interval);
    RUN_TEST(test_a_site_not_repeating_is_never_due);
    RUN_TEST(test_json_holds_the_counts_and_the_repeating_sites);
    RUN_TEST(test_json_that_fits_exactly_is_written_whole_and_no_further);
    RUN_TEST(test_json_one_byte_short_for_a_site_leaves_it_out_whole);
    RUN_TEST(test_json_with_no_room_for_the_counts_is_empty);
    RUN_TEST(test_repeats_spread_across_the_whole_window_count);
    RUN_TEST(test_repeats_spread_one_frame_wider_than_the_window_do_not);
    RUN_TEST(test_sites_taking_turns_are_not_merged);
    RUN_TEST(test_a_site_keeps_one_slot_when_an_earlier_slot_frees_up);
    RUN_TEST(test_settling_forgets_a_site_and_restarts_the_warmup);
    RUN_TEST(test_the_verdict_counts_what_became_repeating_and_what_was_dropped);
#ifdef DEVICE_BUILD
    RUN_TEST(test_on_the_board_an_allocation_every_present_is_caught);
    RUN_TEST(test_on_the_board_a_log_line_every_present_is_caught);
    RUN_TEST(test_on_the_board_one_allocation_among_many_presents_is_not);
    RUN_TEST(test_on_the_board_three_allocation_sites_taking_turns_are_not_merged);
    RUN_TEST(test_on_the_board_three_log_lines_taking_turns_are_not_merged);
#endif
}

SUITE_REGISTER(suite_frame_watch);
