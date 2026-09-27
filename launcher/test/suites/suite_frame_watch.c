/*
 * Portable suite: frame_watch - a call site that repeats across frames,
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

static void
test_json_too_short_for_a_site_leaves_it_out_whole(void) {
    fixture();
    pass_warmup();
    note_every(1, FRAME_WATCH_REPEATS);
    char json[96];
    const int length = frame_watch_format_json(&watch, json, sizeof json);
    TEST_ASSERT_EQUAL_INT((int)strlen(json), length);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"sites\":[]}"));
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

/* The RUN_TEST wrapper already watches this test; its verdict is taken here
 * and the watch started again, so the wrapper's own finds nothing. */
static int
sites_caught_so_far(void) {
    const int caught = frame_watch_test_end();
    frame_watch_test_begin();
    return caught;
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
    RUN_TEST(test_json_too_short_for_a_site_leaves_it_out_whole);
#ifdef DEVICE_BUILD
    RUN_TEST(test_on_the_board_an_allocation_every_present_is_caught);
    RUN_TEST(test_on_the_board_a_log_line_every_present_is_caught);
    RUN_TEST(test_on_the_board_one_allocation_among_many_presents_is_not);
#endif
}

SUITE_REGISTER(suite_frame_watch);
