/*
 * Portable suite: sand_menu - what a title or options tap means, and the
 * draft/commit split that keeps an unapplied option out of the next run.
 */

#include "suites.h"
#include "unity.h"

#include "sand_menu.h"

static const sand_options_t STARTING = {
    .quality = 2,
    .color = SAND_COLOUR_256,
    .dither = 2,
};

static sand_menu_t menu;

static void
fixture(void) {
    sand_menu_init(&menu, STARTING);
}

static void
open_options(void) {
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_OPTIONS, menu.screen);
}

static sand_options_hits_t
hits(void) {
    return SAND_OPTIONS_NO_HITS;
}

static bool
step_quality(int quality) {
    sand_options_hits_t h = hits();
    h.quality = quality;
    return sand_menu_options_step(&menu, h);
}

static bool
step_color(sand_colour_mode_t color) {
    sand_options_hits_t h = hits();
    h.color = (int)color;
    return sand_menu_options_step(&menu, h);
}

static bool
step_dither(int dither) {
    sand_options_hits_t h = hits();
    h.dither = dither;
    return sand_menu_options_step(&menu, h);
}

static bool
step_apply(void) {
    sand_options_hits_t h = hits();
    h.apply = true;
    return sand_menu_options_step(&menu, h);
}

static bool
step_cancel(void) {
    sand_options_hits_t h = hits();
    h.cancel = true;
    return sand_menu_options_step(&menu, h);
}

static void
assert_options_equal(sand_options_t want, sand_options_t got) {
    TEST_ASSERT_EQUAL_INT(want.quality, got.quality);
    TEST_ASSERT_EQUAL_INT(want.color, got.color);
    TEST_ASSERT_EQUAL_INT(want.dither, got.dither);
}

static void
test_the_menu_opens_on_the_title_with_nothing_pending(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
    assert_options_equal(STARTING, menu.committed);
    TEST_ASSERT_EQUAL_INT(0, sand_menu_pending_changes(&menu));
}

static void
test_start_and_exit_are_the_two_title_taps_the_app_acts_on(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(SAND_MENU_START, sand_menu_title_clicked(&menu, SAND_TITLE_START));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_EXIT, sand_menu_title_clicked(&menu, SAND_TITLE_EXIT));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_NONE));
}

static void
test_load_saves_and_guide_have_no_screen_yet_so_nothing_moves(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_LOAD));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_GUIDE));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
}

static void
test_an_edit_changes_the_draft_and_never_the_committed_options(void) {
    fixture();
    open_options();

    TEST_ASSERT_FALSE(step_quality(0));
    TEST_ASSERT_FALSE(step_color(SAND_COLOUR_FULL));

    TEST_ASSERT_EQUAL_INT(0, menu.draft.quality);
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, menu.draft.color);
    assert_options_equal(STARTING, menu.committed);
    TEST_ASSERT_EQUAL_INT(2, sand_menu_pending_changes(&menu));
}

static void
test_apply_commits_the_draft_and_returns_to_the_title(void) {
    fixture();
    open_options();
    step_quality(4);

    TEST_ASSERT_TRUE_MESSAGE(step_apply(), "APPLY is the one moment the app must adopt the options");
    TEST_ASSERT_EQUAL_INT(4, menu.committed.quality);
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
    TEST_ASSERT_EQUAL_INT(0, sand_menu_pending_changes(&menu));
}

static void
test_apply_with_nothing_pending_does_nothing(void) {
    fixture();
    open_options();

    TEST_ASSERT_FALSE(step_apply());
    TEST_ASSERT_EQUAL_INT(SAND_MENU_OPTIONS, menu.screen);
}

static void
test_cancel_drops_the_draft_so_reopening_starts_from_the_committed_options(void) {
    fixture();
    open_options();
    step_quality(0);
    step_color(SAND_COLOUR_16);

    TEST_ASSERT_FALSE(step_cancel());
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
    assert_options_equal(STARTING, menu.committed);

    open_options();
    assert_options_equal(STARTING, menu.draft);
    TEST_ASSERT_EQUAL_INT(0, sand_menu_pending_changes(&menu));
}

static void
test_editing_back_to_the_committed_value_leaves_nothing_pending(void) {
    fixture();
    open_options();
    step_quality(0);
    step_quality(STARTING.quality);

    TEST_ASSERT_EQUAL_INT(0, sand_menu_pending_changes(&menu));
}

static void
test_only_sixteen_colours_uses_a_dither(void) {
    TEST_ASSERT_TRUE(sand_menu_dither_applies(SAND_COLOUR_16));
    TEST_ASSERT_FALSE(sand_menu_dither_applies(SAND_COLOUR_256));
    TEST_ASSERT_FALSE(sand_menu_dither_applies(SAND_COLOUR_FULL));
}

static void
test_a_dither_the_screen_no_longer_shows_is_neither_pending_nor_applied(void) {
    fixture();
    open_options();
    step_color(SAND_COLOUR_16);
    step_dither(4);
    TEST_ASSERT_EQUAL_INT(2, sand_menu_pending_changes(&menu));

    step_color(SAND_COLOUR_FULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_menu_pending_changes(&menu),
                                  "APPLY must not count a dither pick hidden with its rows");

    TEST_ASSERT_TRUE(step_apply());
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, menu.committed.color);
    TEST_ASSERT_EQUAL_INT(STARTING.dither, menu.committed.dither);
}

void
run_sand_menu_suite(void) {
    RUN_TEST(test_the_menu_opens_on_the_title_with_nothing_pending);
    RUN_TEST(test_start_and_exit_are_the_two_title_taps_the_app_acts_on);
    RUN_TEST(test_load_saves_and_guide_have_no_screen_yet_so_nothing_moves);
    RUN_TEST(test_an_edit_changes_the_draft_and_never_the_committed_options);
    RUN_TEST(test_apply_commits_the_draft_and_returns_to_the_title);
    RUN_TEST(test_apply_with_nothing_pending_does_nothing);
    RUN_TEST(test_cancel_drops_the_draft_so_reopening_starts_from_the_committed_options);
    RUN_TEST(test_editing_back_to_the_committed_value_leaves_nothing_pending);
    RUN_TEST(test_only_sixteen_colours_uses_a_dither);
    RUN_TEST(test_a_dither_the_screen_no_longer_shows_is_neither_pending_nor_applied);
}

SUITE_REGISTER(run_sand_menu_suite);
