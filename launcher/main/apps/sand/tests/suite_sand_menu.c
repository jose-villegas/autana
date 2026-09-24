/*
 * Portable suite: sand_menu - what a title or options tap means, and the
 * draft that keeps an unapplied option out of the next run. `committed`
 * stands in for the app's own options: it adopts the draft only when a step
 * says APPLY committed it, exactly as the app does.
 */

#include "suites.h"
#include "unity.h"

#include "apps/sand/sand_menu.h"

static const sand_options_t STARTING = {
    .quality = 2,
    .color = SAND_COLOUR_256,
    .dither = 2,
};

static sand_menu_t menu;
static sand_options_t committed;

static void
fixture(void) {
    committed = STARTING;
    sand_menu_init(&menu);
}

static void
open_options(void) {
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_OPTIONS, committed));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_OPTIONS, menu.screen);
}

static bool
step(sand_options_hits_t hits) {
    const bool applied = sand_menu_options_step(&menu, committed, hits);
    if (applied) {
        committed = menu.draft;
    }
    return applied;
}

static bool
step_quality(int quality) {
    sand_options_hits_t h = SAND_OPTIONS_NO_HITS;
    h.quality = quality;
    return step(h);
}

static bool
step_color(sand_colour_mode_t color) {
    sand_options_hits_t h = SAND_OPTIONS_NO_HITS;
    h.color = (int)color;
    return step(h);
}

static bool
step_dither(int dither) {
    sand_options_hits_t h = SAND_OPTIONS_NO_HITS;
    h.dither = dither;
    return step(h);
}

static bool
step_apply(void) {
    sand_options_hits_t h = SAND_OPTIONS_NO_HITS;
    h.apply = true;
    return step(h);
}

static bool
step_cancel(void) {
    sand_options_hits_t h = SAND_OPTIONS_NO_HITS;
    h.cancel = true;
    return step(h);
}

static void
assert_options_equal(sand_options_t want, sand_options_t got) {
    TEST_ASSERT_EQUAL_INT(want.quality, got.quality);
    TEST_ASSERT_EQUAL_INT(want.color, got.color);
    TEST_ASSERT_EQUAL_INT(want.dither, got.dither);
}

static void
test_the_menu_opens_on_the_title(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
}

static void
test_start_and_exit_are_the_two_title_taps_the_app_acts_on(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(SAND_MENU_START, sand_menu_title_clicked(&menu, SAND_TITLE_START, committed));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_EXIT, sand_menu_title_clicked(&menu, SAND_TITLE_EXIT, committed));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_NONE, committed));
}

static void
test_load_saves_and_guide_have_no_screen_yet_so_nothing_moves(void) {
    fixture();
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_LOAD, committed));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
    TEST_ASSERT_EQUAL_INT(SAND_MENU_STAY, sand_menu_title_clicked(&menu, SAND_TITLE_GUIDE, committed));
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
}

static void
test_opening_the_options_starts_the_draft_from_the_committed_options(void) {
    fixture();
    open_options();
    assert_options_equal(STARTING, menu.draft);
    TEST_ASSERT_EQUAL_INT(0, sand_menu_pending_changes(&menu, committed));
}

static void
test_an_edit_changes_the_draft_and_never_the_committed_options(void) {
    fixture();
    open_options();

    TEST_ASSERT_FALSE(step_quality(0));
    TEST_ASSERT_FALSE(step_color(SAND_COLOUR_FULL));

    TEST_ASSERT_EQUAL_INT(0, menu.draft.quality);
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, menu.draft.color);
    assert_options_equal(STARTING, committed);
    TEST_ASSERT_EQUAL_INT(2, sand_menu_pending_changes(&menu, committed));
}

static void
test_apply_hands_back_the_draft_and_returns_to_the_title(void) {
    fixture();
    open_options();
    step_quality(4);

    TEST_ASSERT_TRUE_MESSAGE(step_apply(), "APPLY is the one moment the app must adopt the draft");
    TEST_ASSERT_EQUAL_INT(4, committed.quality);
    TEST_ASSERT_EQUAL_INT(SAND_MENU_TITLE, menu.screen);
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
    assert_options_equal(STARTING, committed);

    open_options();
    assert_options_equal(STARTING, menu.draft);
}

static void
test_editing_back_to_the_committed_value_leaves_nothing_pending(void) {
    fixture();
    open_options();
    step_quality(0);
    step_quality(STARTING.quality);

    TEST_ASSERT_EQUAL_INT(0, sand_menu_pending_changes(&menu, committed));
}

static void
test_only_sixteen_colours_uses_a_dither(void) {
    TEST_ASSERT_TRUE(sand_menu_dither_applies(SAND_COLOUR_16));
    TEST_ASSERT_FALSE(sand_menu_dither_applies(SAND_COLOUR_256));
    TEST_ASSERT_FALSE(sand_menu_dither_applies(SAND_COLOUR_FULL));
}

static void
test_a_dither_picked_under_sixteen_colours_is_committed(void) {
    fixture();
    open_options();
    step_color(SAND_COLOUR_16);
    step_dither(4);

    TEST_ASSERT_TRUE(step_apply());
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_16, committed.color);
    TEST_ASSERT_EQUAL_INT_MESSAGE(4, committed.dither, "the pick the screen showed must survive APPLY");
}

static void
test_a_dither_the_screen_no_longer_shows_is_neither_pending_nor_applied(void) {
    fixture();
    open_options();
    step_color(SAND_COLOUR_16);
    step_dither(4);
    TEST_ASSERT_EQUAL_INT(2, sand_menu_pending_changes(&menu, committed));

    step_color(SAND_COLOUR_FULL);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, sand_menu_pending_changes(&menu, committed),
                                  "APPLY must not count a dither pick hidden with its control");

    TEST_ASSERT_TRUE(step_apply());
    TEST_ASSERT_EQUAL_INT(SAND_COLOUR_FULL, committed.color);
    TEST_ASSERT_EQUAL_INT(STARTING.dither, committed.dither);
}

void
run_sand_menu_suite(void) {
    RUN_TEST(test_the_menu_opens_on_the_title);
    RUN_TEST(test_start_and_exit_are_the_two_title_taps_the_app_acts_on);
    RUN_TEST(test_load_saves_and_guide_have_no_screen_yet_so_nothing_moves);
    RUN_TEST(test_opening_the_options_starts_the_draft_from_the_committed_options);
    RUN_TEST(test_an_edit_changes_the_draft_and_never_the_committed_options);
    RUN_TEST(test_apply_hands_back_the_draft_and_returns_to_the_title);
    RUN_TEST(test_apply_with_nothing_pending_does_nothing);
    RUN_TEST(test_cancel_drops_the_draft_so_reopening_starts_from_the_committed_options);
    RUN_TEST(test_editing_back_to_the_committed_value_leaves_nothing_pending);
    RUN_TEST(test_only_sixteen_colours_uses_a_dither);
    RUN_TEST(test_a_dither_picked_under_sixteen_colours_is_committed);
    RUN_TEST(test_a_dither_the_screen_no_longer_shows_is_neither_pending_nor_applied);
}

SUITE_REGISTER(run_sand_menu_suite);
