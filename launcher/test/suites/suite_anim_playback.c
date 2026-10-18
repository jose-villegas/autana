#include <math.h>
#include <stdlib.h>
#include "anim/anim_skeleton_format.h"
#include "anim/playback/anim_layer.h"
#include "suites.h"
#include "unity.h"

static anim_layer_t
fixture(void) {
    static const anim_clip_t CLIPS[] = {{1000}, {2000}};
    anim_layer_t layer;
    transformf_t* shown = calloc(1, sizeof *shown);
    transformf_t* frozen = calloc(1, sizeof *frozen);
    TEST_ASSERT_NOT_NULL(shown);
    TEST_ASSERT_NOT_NULL(frozen);
    anim_layer_init(&layer, CLIPS, sizeof CLIPS / sizeof CLIPS[0], (anim_pose_t){shown, 1}, (anim_pose_t){frozen, 1});
    return layer;
}

static void
release(anim_layer_t* l) {
    free(l->shown.local);
    free(l->frozen.local);
}

static void
test_playhead_long_loop_clamp_pause_and_fractional_speed(void) {
    anim_layer_t l = fixture();
    anim_playhead_t* p = &l.current.playhead;
    anim_playhead_init(p, ANIM_LOOP);
    anim_playhead_advance(p, &l.clips[0], 3999999001U);
    TEST_ASSERT_EQUAL_UINT32(1, p->t_ms);
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, 0.001F, anim_playhead_seconds(p));
    anim_playhead_advance(p, &l.clips[0], UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(296, p->t_ms);
    TEST_ASSERT_TRUE(anim_playhead_set_speed(p, 0.5F));
    anim_playhead_advance(p, &l.clips[0], 1);
    TEST_ASSERT_EQUAL_UINT32(296, p->t_ms);
    anim_playhead_advance(p, &l.clips[0], 1);
    TEST_ASSERT_EQUAL_UINT32(297, p->t_ms);
    p->paused = true;
    anim_playhead_advance(p, &l.clips[0], UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(297, p->t_ms);
    TEST_ASSERT_FALSE(anim_playhead_set_speed(p, -1.0F));
    TEST_ASSERT_FALSE(anim_playhead_set_speed(p, NAN));
    anim_playhead_init(p, ANIM_CLAMP);
    anim_playhead_advance(p, &l.clips[0], UINT32_MAX);
    TEST_ASSERT_EQUAL_UINT32(1000, p->t_ms);
    const anim_clip_t empty = {0};
    anim_playhead_advance(p, &empty, 0);
    TEST_ASSERT_EQUAL_FLOAT(0, anim_playhead_seconds(p));
    release(&l);
}

static void
test_transition_blend_weights_and_both_slot_times(void) {
    anim_layer_t l = fixture();
    anim_layer_play(&l, 0, 1000);
    TEST_ASSERT_EQUAL_FLOAT(1, anim_layer_weight(&l));
    TEST_ASSERT_EQUAL_UINT8(ANIM_LAYER_CLIP_NONE, l.previous.clip);
    anim_layer_advance(&l, 100);
    anim_layer_play(&l, 1, 200);
    TEST_ASSERT_EQUAL_FLOAT(0, anim_layer_weight(&l));
    TEST_ASSERT_TRUE(anim_playhead_set_speed(&l.current.playhead, 2.0F));
    anim_layer_advance(&l, 100);
    TEST_ASSERT_EQUAL_FLOAT(0.5F, anim_layer_weight(&l));
    TEST_ASSERT_EQUAL_UINT32(200, l.current.playhead.t_ms);
    TEST_ASSERT_EQUAL_UINT32(200, l.previous.playhead.t_ms);
    anim_layer_advance(&l, UINT32_MAX);
    TEST_ASSERT_EQUAL_FLOAT(1, anim_layer_weight(&l));
    TEST_ASSERT_EQUAL_UINT8(ANIM_LAYER_CLIP_NONE, l.previous.clip);
    release(&l);
}

static void
test_transition_blend_interruption_is_continuous(void) {
    anim_layer_t l = fixture();
    static const anim_skeleton_joint_t JOINT = {0, ANIM_SKELETON_ROOT, 0};
    static const float REST[][ANIM_SKELETON_REST_WIDTH] = {{0, 0, 0, 0, 0, 0, 1, 1, 1, 1}};
    static const anim_skeleton_t SKEL = {1, &JOINT, REST, "root"};
    static const float TIMES[] = {0}, A[] = {0, 0, 0}, B[] = {10, 0, 0};
    const anim_joint_bound_t a = {{TIMES, A, 1, 3, ANIM_STEP, 0}, 0, ANIM_JOINT_POSITION};
    const anim_joint_bound_t b = {{TIMES, B, 1, 3, ANIM_STEP, 0}, 0, ANIM_JOINT_POSITION};
    const anim_joint_bound_t* const bound[] = {&a, &b};
    const int counts[] = {1, 1};
    anim_layer_play(&l, 0, 0);
    (void)anim_layer_pose(&l, &SKEL, bound, counts);
    anim_layer_play(&l, 1, 200);
    anim_layer_advance(&l, 100);
    const anim_pose_t* shown = anim_layer_pose(&l, &SKEL, bound, counts);
    TEST_ASSERT_EQUAL_FLOAT(5, shown->local[0].position.x);
    anim_layer_play(&l, 0, 200);
    TEST_ASSERT_TRUE(l.frozen_previous);
    shown = anim_layer_pose(&l, &SKEL, bound, counts);
    TEST_ASSERT_EQUAL_FLOAT(5, shown->local[0].position.x);
    anim_layer_advance(&l, 100);
    shown = anim_layer_pose(&l, &SKEL, bound, counts);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, shown->local[0].position.x);
    anim_layer_play(&l, 1, 200);
    shown = anim_layer_pose(&l, &SKEL, bound, counts);
    TEST_ASSERT_EQUAL_FLOAT(2.5F, shown->local[0].position.x);
    anim_layer_play(&l, 1, 0);
    shown = anim_layer_pose(&l, &SKEL, bound, counts);
    TEST_ASSERT_EQUAL_FLOAT(10, shown->local[0].position.x);
    TEST_ASSERT_FALSE(l.frozen_previous);
    release(&l);
}

static void
test_speed_accepts_uint16_max_and_refuses_larger_or_infinite(void) {
    anim_layer_t l = fixture();
    anim_playhead_t* p = &l.current.playhead;
    TEST_ASSERT_TRUE(anim_playhead_set_speed(p, (float)UINT16_MAX));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)UINT16_MAX * ANIM_PLAYHEAD_SPEED_ONE, p->speed_q16);
    TEST_ASSERT_FALSE(anim_playhead_set_speed(p, nextafterf((float)UINT16_MAX, INFINITY)));
    TEST_ASSERT_FALSE(anim_playhead_set_speed(p, INFINITY));
    release(&l);
}

void
suite_anim_playback(void) {
    RUN_TEST(test_speed_accepts_uint16_max_and_refuses_larger_or_infinite);
    RUN_TEST(test_playhead_long_loop_clamp_pause_and_fractional_speed);
    RUN_TEST(test_transition_blend_weights_and_both_slot_times);
    RUN_TEST(test_transition_blend_interruption_is_continuous);
}

SUITE_REGISTER(suite_anim_playback);
