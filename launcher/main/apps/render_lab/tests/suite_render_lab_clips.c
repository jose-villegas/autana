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
    return f;
}

static void
test_console_clip_selection_and_validation(void) {
    clip_fixture_t* f = fixture();
    TEST_ASSERT_TRUE(render_lab_clips_console(&f->scene, 0, &f->choice, "clip gallop 300"));
    TEST_ASSERT_EQUAL_INT(1, f->choice.selected);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->choice.blend_ms);
    TEST_ASSERT_EQUAL_INT(1, f->animator.layer.current.clip);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->animator.layer.blend_ms);
    static const char* const bad[] = {
        "clip missing", "clip idle -1", "clip idle 1001", "clip idle 12x", "clip idle 999999999999999999999999",
        "clip idle ",   "clip"};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        TEST_ASSERT_TRUE(render_lab_clips_console(&f->scene, 0, &f->choice, bad[i]));
        TEST_ASSERT_EQUAL_INT(1, f->choice.selected);
        TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->choice.blend_ms);
    }
    TEST_ASSERT_TRUE(render_lab_clips_console(NULL, 0, &f->choice, "clip idle"));
    TEST_ASSERT_TRUE(render_lab_clips_console(&f->scene, 0, &f->choice, "clips"));
    TEST_ASSERT_TRUE(render_lab_clips_console(&f->scene, 0, &f->choice, "clip idle"));
    TEST_ASSERT_EQUAL_INT(0, f->choice.selected);
    TEST_ASSERT_EQUAL_INT(TEST_BLEND_MS, f->animator.layer.blend_ms);
    TEST_ASSERT_FALSE(render_lab_clips_console(&f->scene, 0, &f->choice, "scene capybara"));
    TEST_ASSERT_FALSE(render_lab_clips_play(&f->scene, 0, &f->choice, 1, RENDER_LAB_BLEND_MAX_MS + 1));
}

static void
test_picker_items_live_until_scene_exit(void) {
    clip_fixture_t* f = fixture();
    const size_t free_before = memory_free_bytes(MEMORY_PSRAM);
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->scene, 0, &f->choice));
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
static bool dropdown_open;

static void
clip_controls(mu_Context* ctx, ui_flow_t* flow, int width) {
    clip_rect = (mu_Rect){(flow->canvas_w - width) / 2, flow->y + gfx_text_height() + flow->gap, width, UI_ROW_HEIGHT};
    render_lab_clips_draw(ctx, flow, width, &shown->scene, 0, &shown->choice);
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
test_menu_dropdown_plays_with_selected_blend(void) {
    clip_fixture_t* f = fixture();
    f->choice.blend_ms = TEST_BLEND_MS;
    TEST_ASSERT_TRUE(render_lab_clips_init(&f->scene, 0, &f->choice));
    const ui_dropdown_item_t* items = f->choice.items;
    const size_t free_before = memory_free_bytes(MEMORY_PSRAM);
    ui_init();
    menu_frame(NULL);
    menu_frame(NULL);
    TEST_ASSERT_FALSE(dropdown_open);
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

void
run_render_lab_clips_suite(void) {
    RUN_TEST(test_picker_items_live_until_scene_exit);
    RUN_TEST(test_console_clip_selection_and_validation);
    RUN_TEST(test_menu_calls_optional_controls);
    RUN_TEST(test_menu_dropdown_plays_with_selected_blend);
}

SUITE_REGISTER(run_render_lab_clips_suite);
