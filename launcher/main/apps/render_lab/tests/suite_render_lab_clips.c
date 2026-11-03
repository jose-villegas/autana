#include <stdio.h>
#include <string.h>
#include "anim/anim_tracks_format.h"
#include "app/app_arena.h"
#include "apps/render_lab/render_lab_clips.h"
#include "apps/render_lab/ui/render_lab_menu_screen.h"
#include "collections/strbuf.h"
#include "core/memory.h"
#include "gfx/gfx.h"
#include "scene/scene_internal.h"
#include "suites.h"
#include "testkit/cleanup.h"
#include "ui/ui.h"
#include "ui_probe.h"
#include "unity.h"

#define TEST_BLEND_MS   300
#define TEST_CLIP_COUNT 2

typedef struct {
    scene_animator_clip_t clips[TEST_CLIP_COUNT];
    scene_animator_t animator;
    scene_t scene;
    transformf_t shown_pose, frozen_pose;
    render_lab_clips_t choice;
    size_t arena_mark; /* the arena before the test, given back after it */
    char reply[(TEST_CLIP_COUNT * (ASSET_NAME_MAX + sizeof("RENDER clip= current=1\n"))) + sizeof("RENDER_END\n")];
    char long_name[ASSET_NAME_MAX + 1];
} clip_fixture_t;

static clip_fixture_t* shown;

static void
release_fixture(void) {
#ifndef ESP_PLATFORM
    ui_probe_widgets_stop();
#endif
    render_lab_clips_destroy(&shown->choice);
    app_arena_rewind(shown->arena_mark);
    memory_free(shown);
    shown = NULL;
}

static clip_fixture_t*
fixture(void) {
    clip_fixture_t* f = memory_alloc(sizeof *f, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(f);
    shown = f;
    suite_set_test_cleanup(release_fixture);
    static const anim_clip_t times[TEST_CLIP_COUNT] = {{0}, {0}};
    *f = (clip_fixture_t){.clips = {{.id = "idle"}, {.id = "gallop"}},
                          .choice = {.blend_ms = RENDER_LAB_BLEND_DEFAULT_MS},
                          .arena_mark = app_arena_mark()};
    f->animator = (scene_animator_t){.storage = f->clips, .clips = f->clips, .root = ANIM_ROOT_SKELETON};
    anim_layer_init(&f->animator.layer, times, TEST_CLIP_COUNT, (anim_pose_t){&f->shown_pose, 1},
                    (anim_pose_t){&f->frozen_pose, 1});
    f->scene = (scene_t){.asset = {.entity_count = 1}, .animators = &f->animator};
    TEST_ASSERT_TRUE(scene_animator_play(&f->scene, 0, 0, 0));
    f->choice.scene = &f->scene;
    f->choice.subject = 0;
    return f;
}

static void
capture_reply(const char* line) {
    strbuf_t reply = strbuf_at(shown->reply, sizeof shown->reply, strlen(shown->reply));
    TEST_ASSERT_TRUE(strbuf_appendf(&reply, "%s\n", line));
}

static void
request(clip_fixture_t* f, const char* args) {
    f->reply[0] = '\0';
    TEST_ASSERT_TRUE(render_lab_clips_reply(&f->choice, args, capture_reply));
}

static void
reject(clip_fixture_t* f, const char* args, const char* expected) {
    const int selected = f->choice.selected;
    const int blend_ms = f->choice.blend_ms;
    const anim_layer_t layer = f->animator.layer;
    request(f, args);
    TEST_ASSERT_EQUAL_STRING(expected, f->reply);
    TEST_ASSERT_EQUAL_INT(selected, f->choice.selected);
    TEST_ASSERT_EQUAL_INT(blend_ms, f->choice.blend_ms);
    TEST_ASSERT_EQUAL_MEMORY(&layer, &f->animator.layer, sizeof layer);
}

static void
test_listing_marks_default_and_fallback_playback(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->choice, &f->scene, 0, "gallop"));
    TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
    request(f, "clips");
    TEST_ASSERT_EQUAL_STRING("RENDER clip=idle current=0\nRENDER clip=gallop current=1\nRENDER_END\n", f->reply);
    render_lab_clips_destroy(&f->choice);
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->choice, &f->scene, 0, "missing"));
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_BLEND_DEFAULT_MS, f->choice.blend_ms);
    TEST_ASSERT_EQUAL_INT(0, f->animator.layer.current.clip);
    request(f, "clips");
    TEST_ASSERT_EQUAL_STRING("RENDER clip=idle current=1\nRENDER clip=gallop current=0\nRENDER_END\n", f->reply);
}

static void
test_console_success_reports_applied_blend_and_end(void) {
    clip_fixture_t* f = fixture();
    char command[sizeof("clip gallop ") + RENDER_LAB_BLEND_TEXT_SIZE];
    char expected[ASSET_NAME_MAX + sizeof("RENDER clip= blend_ms=\nRENDER_END\n") + RENDER_LAB_BLEND_TEXT_SIZE];
    TEST_ASSERT_TRUE(snprintf(command, sizeof command, "clip gallop %d", TEST_BLEND_MS) >= 0);
    TEST_ASSERT_TRUE(snprintf(expected, sizeof expected, "RENDER clip=gallop blend_ms=%d\nRENDER_END\n", TEST_BLEND_MS)
                     >= 0);
    request(f, command);
    TEST_ASSERT_EQUAL_STRING(expected, f->reply);
    TEST_ASSERT_TRUE(snprintf(expected, sizeof expected, "RENDER clip=idle blend_ms=%d\nRENDER_END\n", TEST_BLEND_MS)
                     >= 0);
    request(f, "clip idle");
    TEST_ASSERT_EQUAL_STRING(expected, f->reply);
    TEST_ASSERT_EQUAL_UINT32(TEST_BLEND_MS, f->animator.layer.blend_ms);
}

static void
test_init_plays_without_blend_and_console_uses_default(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->choice, &f->scene, 0, "idle"));
    TEST_ASSERT_EQUAL_UINT32(0, f->animator.layer.blend_ms);
    request(f, "clip gallop");
    TEST_ASSERT_EQUAL_UINT32(RENDER_LAB_BLEND_DEFAULT_MS, f->animator.layer.blend_ms);
}

static void
test_listing_rejects_extra_arguments(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_FALSE(render_lab_clips_reply(&f->choice, "clips extra", capture_reply));
    TEST_ASSERT_EQUAL_STRING("", f->reply);
}

static void
test_console_errors_leave_playback_unchanged(void) {
    clip_fixture_t* f = fixture();
    char selection[sizeof("clip gallop ") + RENDER_LAB_BLEND_TEXT_SIZE];
    TEST_ASSERT_TRUE(snprintf(selection, sizeof selection, "clip gallop %d", TEST_BLEND_MS) >= 0);
    request(f, selection);
    reject(f, "clip missing", "RENDER_ERR unknown clip 'missing'\n");
    reject(f, "clip", "RENDER_ERR bad clip name\n");
    static const char* const bad_blends[] = {"-1", "12x", "999999999999999999999999", ""};
    char command[ASSET_NAME_MAX + sizeof("clip idle ")];
    for (size_t i = 0; i < sizeof bad_blends / sizeof bad_blends[0]; i++) {
        TEST_ASSERT_TRUE(snprintf(command, sizeof command, "clip idle %s", bad_blends[i]) >= 0);
        reject(f, command, "RENDER_ERR bad blend\n");
    }
    TEST_ASSERT_TRUE(snprintf(command, sizeof command, "clip idle %d", RENDER_LAB_BLEND_MAX_MS + 1) >= 0);
    reject(f, command, "RENDER_ERR bad blend\n");
    f->choice.scene = NULL;
    reject(f, "clips", "RENDER_ERR no animated subject\n");
    f->choice.scene = &f->scene;
    f->animator.storage = NULL;
    reject(f, "clip idle", "RENDER_ERR no animated subject\n");
    f->animator.storage = f->clips;
}

static void
test_console_accepts_name_and_blend_boundaries(void) {
    clip_fixture_t* f = fixture();
    memset(f->long_name, 'x', sizeof f->long_name);
    f->long_name[ASSET_NAME_MAX - 1] = '\0';
    f->clips[1].id = f->long_name;
    static const int blends[] = {0, RENDER_LAB_BLEND_MAX_MS};
    char command[ASSET_NAME_MAX + sizeof("clip  ") + RENDER_LAB_BLEND_TEXT_SIZE];
    for (size_t i = 0; i < sizeof blends / sizeof blends[0]; i++) {
        TEST_ASSERT_TRUE(snprintf(command, sizeof command, "clip %s %d", f->long_name, blends[i]) >= 0);
        request(f, command);
        TEST_ASSERT_EQUAL_INT(1, f->choice.selected);
        TEST_ASSERT_EQUAL_INT(blends[i], f->choice.blend_ms);
        TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
        TEST_ASSERT_EQUAL_UINT32(blends[i], f->animator.layer.blend_ms);
    }
    f->long_name[ASSET_NAME_MAX - 1] = 'x';
    f->long_name[ASSET_NAME_MAX] = '\0';
    TEST_ASSERT_TRUE(snprintf(command, sizeof command, "clip %s", f->long_name) >= 0);
    reject(f, command, "RENDER_ERR bad clip name\n");
}

static void
test_console_matches_case_folded_words(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_TRUE(render_lab_clips_reply(&f->choice, "CLIP gallop 0", capture_reply));
    TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
}

static void
test_console_uses_current_blend_when_omitted(void) {
    clip_fixture_t* f = fixture();
    char command[sizeof("clip gallop ") + RENDER_LAB_BLEND_TEXT_SIZE];
    TEST_ASSERT_TRUE(snprintf(command, sizeof command, "clip gallop %d", TEST_BLEND_MS) >= 0);
    TEST_ASSERT_TRUE(render_lab_clips_reply(&f->choice, command, capture_reply));
    TEST_ASSERT_TRUE(render_lab_clips_reply(&f->choice, "clip idle", capture_reply));
    TEST_ASSERT_EQUAL_INT(0, f->choice.selected);
    TEST_ASSERT_EQUAL_INT(0, f->animator.layer.current.clip);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->choice.blend_ms);
    TEST_ASSERT_EQUAL_UINT32(TEST_BLEND_MS, f->animator.layer.blend_ms);
    TEST_ASSERT_FALSE(render_lab_clips_reply(&f->choice, "scene capybara", capture_reply));
}

static void
test_picker_init_takes_items_from_the_arena(void) {
    clip_fixture_t* f = fixture();
    const size_t psram_before = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->choice, &f->scene, 0, "idle"));
    TEST_ASSERT_EQUAL_INT(TEST_CLIP_COUNT, f->choice.count);
    TEST_ASSERT_NOT_NULL(f->choice.items);
    for (int i = 0; i < TEST_CLIP_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(f->clips[i].id, f->choice.items[i]);
    }
    TEST_ASSERT_GREATER_OR_EQUAL_size_t(f->arena_mark + (TEST_CLIP_COUNT * sizeof *f->choice.items), app_arena_mark());
    TEST_ASSERT_EQUAL_size_t(psram_before, memory_free_bytes(MEMORY_PSRAM));
    render_lab_clips_destroy(&f->choice);
    TEST_ASSERT_NULL(f->choice.items);
    TEST_ASSERT_EQUAL_INT(0, f->choice.count);
}

#ifndef ESP_PLATFORM
static void
test_picker_init_reports_allocation_failure(void) {
    clip_fixture_t* f = fixture();
    ui_probe_widgets_start();
    TEST_ASSERT_NOT_NULL(app_arena_take(APP_ARENA_BYTES - app_arena_mark(), 1)); /* the arena full */
    TEST_ASSERT_FALSE(render_lab_clips_init(&f->choice, &f->scene, 0, "gallop"));
    TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
    request(f, "clips");
    TEST_ASSERT_EQUAL_STRING("RENDER clip=idle current=0\nRENDER clip=gallop current=1\nRENDER_END\n", f->reply);
    request(f, "clip idle");
    TEST_ASSERT_EQUAL_INT(0, f->animator.layer.current.clip);
    ui_init();
    ui_begin(NULL);
    ui_screen();
    render_lab_clips_draw(ui_width(), &f->choice);
    ui_end(UI_NO_BACKGROUND);
    recti_t drawn;
    TEST_ASSERT_FALSE(ui_probe_widget("CLIP", &drawn));
    TEST_ASSERT_FALSE(ui_probe_widget("BLEND", &drawn));
    TEST_ASSERT_FALSE(ui_probe_text("CLIP", &drawn));
    TEST_ASSERT_NULL(f->choice.items);
    TEST_ASSERT_EQUAL_INT(0, f->choice.count);
    TEST_ASSERT_EQUAL_INT(RENDER_LAB_BLEND_DEFAULT_MS, f->choice.blend_ms);
}
#endif

static int controls_drawn;

static void
controls(int width) {
    controls_drawn++;
    ui_row_centre(width, UI_ROW_HEIGHT);
    ui_text(ui_next(), "TEST CONTROL");
}

static void
test_menu_calls_optional_controls(void) {
    ui_init();
    controls_drawn = 0;
    ui_begin(NULL);
    const render_lab_menu_screen_state_t state = {.controls = controls};
    (void)render_lab_menu_screen_draw(&state);
    ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, controls_drawn);
    ui_begin(NULL);
    const render_lab_menu_screen_state_t plain = {0};
    (void)render_lab_menu_screen_draw(&plain);
    ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, controls_drawn);
}

/* The menu tests find the dropdown and slider by label through the host-only
 * widget probe; the device's clip_dispatch suite taps the same menu. */
#ifndef ESP_PLATFORM
static recti_t clip_rect;
static recti_t slider_rect;
static bool dropdown_open;
static recti_t list_rows;
static int list_scroll;

static void
clip_controls(int width) {
    render_lab_clips_draw(width, &shown->choice);
}

static void
menu_frame(const input_t* input) {
    ui_begin(input);
    const render_lab_menu_screen_state_t state = {.controls = clip_controls};
    (void)render_lab_menu_screen_draw(&state);
    ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_TRUE(ui_probe_widget("CLIP", &clip_rect));
    TEST_ASSERT_TRUE(ui_probe_widget("BLEND", &slider_rect));
    dropdown_open = ui_probe_list(&list_rows, &list_scroll);
}

/* Row `i` of the open list, one dropdown height per row. */
static recti_t
list_row(int i) {
    return (recti_t){list_rows.x, list_rows.y + (i * clip_rect.h) - list_scroll, list_rows.w, clip_rect.h};
}

static void
menu_tap(recti_t rect) {
    input_t input = {.down = true, .pressed = true, .point = {rect.x + (rect.w / 2), rect.y + (rect.h / 2)}};
    menu_frame(&input);
    input.pressed = false;
    for (int i = 0; i < UI_DROPDOWN_CLOSE_FRAMES; i++) {
        menu_frame(&input);
    }
    input.down = false;
    input.released = true;
    menu_frame(&input);
    input.released = false;
    menu_frame(&input);
}

static void
slider_drag(int blend_ms) {
    input_t drag = {.down = true,
                    .pressed = true,
                    .point = {slider_rect.x + (slider_rect.w / 2), slider_rect.y + (slider_rect.h / 2)}};
    menu_frame(&drag);
    drag.pressed = false;
    menu_frame(&drag);
    drag.point.x = slider_rect.x + (UI_SLIDER_KNOB_W / 2)
                   + ((slider_rect.w - UI_SLIDER_KNOB_W) * blend_ms / RENDER_LAB_BLEND_MAX_MS);
    menu_frame(&drag);
    menu_frame(&drag);
    drag.down = false;
    drag.released = true;
    menu_frame(&drag);
    menu_frame(NULL);
}

static void
test_menu_dropdown_plays_with_selected_blend(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->choice, &f->scene, 0, "idle"));
    const char* const* items = f->choice.items;
    ui_probe_widgets_start();
    const size_t free_before = memory_free_bytes(MEMORY_PSRAM);
    ui_init();
    menu_frame(NULL);
    menu_frame(NULL);
    TEST_ASSERT_FALSE(dropdown_open);
    slider_drag(TEST_BLEND_MS);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->choice.blend_ms);
    menu_tap(clip_rect);
    TEST_ASSERT_TRUE(dropdown_open);
    menu_tap(list_row(1));
    TEST_ASSERT_EQUAL_INT(1, f->choice.selected);
    TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->animator.layer.blend_ms);
    TEST_ASSERT_EQUAL_PTR(items, f->choice.items);
    TEST_ASSERT_EQUAL(free_before, memory_free_bytes(MEMORY_PSRAM));
}

static void
test_draw_accepts_blend_boundaries_for_next_pick(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->choice, &f->scene, 0, "idle"));
    ui_init();
    ui_probe_widgets_start();
    menu_frame(NULL);
    menu_frame(NULL);
    static const int blends[] = {0, RENDER_LAB_BLEND_MAX_MS};
    for (size_t i = 0; i < sizeof blends / sizeof blends[0]; i++) {
        slider_drag(blends[i]);
        TEST_ASSERT_EQUAL_INT(blends[i], f->choice.blend_ms);
        menu_tap(clip_rect);
        TEST_ASSERT_TRUE(dropdown_open);
        const int picked = 1 - f->choice.selected;
        menu_tap(list_row(picked));
        for (int frame = 0; frame < UI_DROPDOWN_CLOSE_FRAMES; frame++) {
            menu_frame(NULL);
        }
        TEST_ASSERT_FALSE(dropdown_open);
        TEST_ASSERT_EQUAL_INT(picked, f->animator.layer.current.clip);
        TEST_ASSERT_EQUAL_UINT32(blends[i], f->animator.layer.blend_ms);
    }
}

#endif

void
run_render_lab_clips_suite(void) {
    RUN_TEST(test_console_success_reports_applied_blend_and_end);
    RUN_TEST(test_init_plays_without_blend_and_console_uses_default);
    RUN_TEST(test_listing_rejects_extra_arguments);
#ifndef ESP_PLATFORM
    RUN_TEST(test_draw_accepts_blend_boundaries_for_next_pick);
#endif
    RUN_TEST(test_listing_marks_default_and_fallback_playback);
    RUN_TEST(test_console_errors_leave_playback_unchanged);
    RUN_TEST(test_console_accepts_name_and_blend_boundaries);
    RUN_TEST(test_console_matches_case_folded_words);
    RUN_TEST(test_picker_init_takes_items_from_the_arena);
#ifndef ESP_PLATFORM
    RUN_TEST(test_picker_init_reports_allocation_failure);
#endif
    RUN_TEST(test_console_uses_current_blend_when_omitted);
    RUN_TEST(test_menu_calls_optional_controls);
#ifndef ESP_PLATFORM
    RUN_TEST(test_menu_dropdown_plays_with_selected_blend);
#endif
}

SUITE_REGISTER(run_render_lab_clips_suite);
