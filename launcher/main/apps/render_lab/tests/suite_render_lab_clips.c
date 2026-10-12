#include <stdio.h>
#include <string.h>
#include "apps/render_lab/render_lab_clips.h"
#include "apps/render_lab/ui/render_lab_menu_screen.h"
#include "core/memory.h"
#include "gfx/draw/gfx_draw.h"
#include "gfx/gfx.h"
#include "scene/scene_internal.h"
#include "suites.h"
#include "test_cleanup.h"
#include "ui/ui.h"
#include "ui/ui_widgets.h"
#include "unity.h"

#define TEST_BLEND_MS   300
#define TEST_CLIP_COUNT 2

typedef struct {
    scene_animator_clip_t clips[TEST_CLIP_COUNT];
    scene_animator_t animator;
    scene_t scene;
    transformf_t shown_pose, frozen_pose;
    render_lab_clips_t choice;
    char reply[TEST_CLIP_COUNT * (ASSET_NAME_MAX + sizeof("RENDER clip= current=1\n")) + sizeof("RENDER_END\n")];
    char long_name[ASSET_NAME_MAX + 1];
} clip_fixture_t;

static clip_fixture_t* shown;

static void
release_fixture(void) {
    render_lab_clips_destroy(&shown->choice);
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
                          .choice = {.blend_ms = RENDER_LAB_BLEND_DEFAULT_MS}};
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
    const size_t used = strlen(shown->reply);
    TEST_ASSERT_LESS_THAN(sizeof shown->reply - used, strlen(line) + 1);
    (void)snprintf(shown->reply + used, sizeof shown->reply - used, "%s\n", line);
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
    TEST_ASSERT_EQUAL_INT(0, f->animator.layer.current.clip);
    request(f, "clips");
    TEST_ASSERT_EQUAL_STRING("RENDER clip=idle current=1\nRENDER clip=gallop current=0\nRENDER_END\n", f->reply);
}

static void
test_console_errors_leave_playback_unchanged(void) {
    clip_fixture_t* f = fixture();
    char selection[sizeof("clip gallop 1000")];
    (void)snprintf(selection, sizeof selection, "clip gallop %d", TEST_BLEND_MS);
    request(f, selection);
    reject(f, "clip missing", "RENDER_ERR unknown clip 'missing'\n");
    reject(f, "clip", "RENDER_ERR bad clip name\n");
    static const char* const bad_blends[] = {"-1", "12x", "999999999999999999999999", ""};
    char command[ASSET_NAME_MAX + sizeof("clip idle ")];
    for (size_t i = 0; i < sizeof bad_blends / sizeof bad_blends[0]; i++) {
        (void)snprintf(command, sizeof command, "clip idle %s", bad_blends[i]);
        reject(f, command, "RENDER_ERR bad blend\n");
    }
    (void)snprintf(command, sizeof command, "clip idle %d", RENDER_LAB_BLEND_MAX_MS + 1);
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
    char command[ASSET_NAME_MAX + sizeof("clip  1000")];
    for (size_t i = 0; i < sizeof blends / sizeof blends[0]; i++) {
        (void)snprintf(command, sizeof command, "clip %s %d", f->long_name, blends[i]);
        request(f, command);
        TEST_ASSERT_EQUAL_INT(1, f->choice.selected);
        TEST_ASSERT_EQUAL_INT(blends[i], f->choice.blend_ms);
        TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
        TEST_ASSERT_EQUAL_UINT32(blends[i], f->animator.layer.blend_ms);
    }
    f->long_name[ASSET_NAME_MAX - 1] = 'x';
    f->long_name[ASSET_NAME_MAX] = '\0';
    (void)snprintf(command, sizeof command, "clip %s", f->long_name);
    reject(f, command, "RENDER_ERR bad clip name\n");
}

static void
test_console_matches_case_folded_words(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_TRUE(render_lab_clips_console(&f->choice, "CLIP gallop 0"));
    TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
}

static void
test_console_uses_current_blend_when_omitted(void) {
    clip_fixture_t* f = fixture();
    char command[sizeof("clip gallop 1000")];
    (void)snprintf(command, sizeof command, "clip gallop %d", TEST_BLEND_MS);
    TEST_ASSERT_TRUE(render_lab_clips_console(&f->choice, command));
    TEST_ASSERT_TRUE(render_lab_clips_console(&f->choice, "clip idle"));
    TEST_ASSERT_EQUAL_INT(0, f->choice.selected);
    TEST_ASSERT_EQUAL_INT(0, f->animator.layer.current.clip);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->choice.blend_ms);
    TEST_ASSERT_EQUAL_UINT32(TEST_BLEND_MS, f->animator.layer.blend_ms);
    TEST_ASSERT_FALSE(render_lab_clips_console(&f->choice, "scene capybara"));
}

static void
test_picker_init_destroy_releases_items(void) {
    clip_fixture_t* f = fixture();
    const size_t free_before = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->choice, &f->scene, 0, "idle"));
    TEST_ASSERT_EQUAL_INT(TEST_CLIP_COUNT, f->choice.count);
    TEST_ASSERT_NOT_NULL(f->choice.items);
    for (int i = 0; i < TEST_CLIP_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(f->clips[i].id, f->choice.items[i].label);
    }
    TEST_ASSERT_LESS_THAN(free_before, memory_free_bytes(MEMORY_PSRAM));
    render_lab_clips_destroy(&f->choice);
    TEST_ASSERT_NULL(f->choice.items);
    TEST_ASSERT_EQUAL_INT(0, f->choice.count);
    TEST_ASSERT_EQUAL(free_before, memory_free_bytes(MEMORY_PSRAM));
}

static int controls_drawn;

static void
controls(mu_Context* ctx, ui_flow_t* flow, int width) {
    controls_drawn++;
    ui_flow_row(ctx, flow, width, UI_ROW_HEIGHT);
    mu_label(ctx, "TEST CONTROL");
}

static void
test_menu_calls_optional_controls(void) {
    ui_init();
    controls_drawn = 0;
    ui_begin(NULL);
    const render_lab_menu_screen_state_t state = {.scene_name = "Test", .controls = controls};
    (void)render_lab_menu_screen_draw(ui_context(), &state, 0);
    ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, controls_drawn);
    ui_begin(NULL);
    const render_lab_menu_screen_state_t plain = {.scene_name = "Plain"};
    (void)render_lab_menu_screen_draw(ui_context(), &plain, 0);
    ui_end(UI_NO_BACKGROUND);
    TEST_ASSERT_EQUAL_INT(1, controls_drawn);
}

static mu_Rect clip_rect;
static mu_Rect slider_rect;
static bool dropdown_open;

static void
clip_controls(mu_Context* ctx, ui_flow_t* flow, int width) {
    clip_rect = (mu_Rect){(flow->canvas_w - width) / 2, flow->y + gfx_text_height() + flow->gap, width, UI_ROW_HEIGHT};
    render_lab_clips_draw(ctx, flow, width, &shown->choice);
    slider_rect = (mu_Rect){clip_rect.x, flow->y - flow->gap - UI_ROW_HEIGHT, width, UI_ROW_HEIGHT};
    dropdown_open = ui_dropdown_is_open(ctx, "clip");
}

static void
menu_frame(const input_t* input) {
    ui_begin(input);
    const render_lab_menu_screen_state_t state = {.scene_name = "Animated", .controls = clip_controls};
    (void)render_lab_menu_screen_draw(ui_context(), &state, 0);
    ui_end(UI_NO_BACKGROUND);
}

static void
menu_tap(mu_Rect rect) {
    input_t input = {.down = true, .pressed = true, .x = rect.x + rect.w / 2, .y = rect.y + rect.h / 2};
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
    input_t drag = {
        .down = true, .pressed = true, .x = slider_rect.x + slider_rect.w / 2, .y = slider_rect.y + slider_rect.h / 2};
    menu_frame(&drag);
    drag.pressed = false;
    menu_frame(&drag);
    drag.x =
        slider_rect.x + UI_SLIDER_KNOB_W / 2 + (slider_rect.w - UI_SLIDER_KNOB_W) * blend_ms / RENDER_LAB_BLEND_MAX_MS;
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
    const ui_dropdown_item_t* items = f->choice.items;
    const size_t free_before = memory_free_bytes(MEMORY_PSRAM);
    ui_init();
    menu_frame(NULL);
    menu_frame(NULL);
    TEST_ASSERT_FALSE(dropdown_open);
    slider_drag(TEST_BLEND_MS);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->choice.blend_ms);
    menu_tap(clip_rect);
    TEST_ASSERT_TRUE(dropdown_open);
    mu_Rect list = ui_dropdown_list_rect(clip_rect, TEST_CLIP_COUNT, clip_rect.h, ui_height(), UI_MARGIN);
    list.y += clip_rect.h;
    list.h = clip_rect.h;
    menu_tap(list);
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
    menu_frame(NULL);
    menu_frame(NULL);
    static const int blends[] = {0, RENDER_LAB_BLEND_MAX_MS};
    for (size_t i = 0; i < sizeof blends / sizeof blends[0]; i++) {
        slider_drag(blends[i]);
        TEST_ASSERT_EQUAL_INT(blends[i], f->choice.blend_ms);
        menu_tap(clip_rect);
        TEST_ASSERT_TRUE(dropdown_open);
        mu_Rect list = ui_dropdown_list_rect(clip_rect, TEST_CLIP_COUNT, clip_rect.h, ui_height(), UI_MARGIN);
        const int picked = 1 - f->choice.selected;
        list.y += picked * clip_rect.h;
        list.h = clip_rect.h;
        menu_tap(list);
        for (int frame = 0; frame < UI_DROPDOWN_CLOSE_FRAMES; frame++) {
            menu_frame(NULL);
        }
        TEST_ASSERT_FALSE(dropdown_open);
        TEST_ASSERT_EQUAL_INT(picked, f->animator.layer.current.clip);
        TEST_ASSERT_EQUAL_UINT32(blends[i], f->animator.layer.blend_ms);
    }
}

void
run_render_lab_clips_suite(void) {
    RUN_TEST(test_draw_accepts_blend_boundaries_for_next_pick);
    RUN_TEST(test_listing_marks_default_and_fallback_playback);
    RUN_TEST(test_console_errors_leave_playback_unchanged);
    RUN_TEST(test_console_accepts_name_and_blend_boundaries);
    RUN_TEST(test_console_matches_case_folded_words);
    RUN_TEST(test_picker_init_destroy_releases_items);
    RUN_TEST(test_console_uses_current_blend_when_omitted);
    RUN_TEST(test_menu_calls_optional_controls);
    RUN_TEST(test_menu_dropdown_plays_with_selected_blend);
}

SUITE_REGISTER(run_render_lab_clips_suite);
