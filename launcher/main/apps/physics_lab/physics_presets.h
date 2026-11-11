/* physics_presets: fixed, timed loads shared by the scene table and its suite. */
#pragma once

/* X(ID, key, ...): one fixed, timed load per row. */
#define PHYSICS_PRESETS(X)
#define PHYSICS_PRESET_ID(id_, ...) PHYSICS_PRESET_##id_,

typedef enum { PHYSICS_PRESETS(PHYSICS_PRESET_ID) PHYSICS_PRESET_COUNT } physics_preset_t;

/* Scene table entries, in preset order. */
#define PHYSICS_PRESET_SCENE_ENTRY(id_, ...) &scene_physics_##id_,
/* One count per row, independent of its fields. */
#define PHYSICS_PRESET_COUNT_ROW(...)        +1

/* A timed suite case for each row's scene key and duration. */
#define PHYSICS_PRESET_CASE(id_, key_, duration_ms_, ...)                                                              \
    static void test_physics_##id_(void) { profile_preset(key_, duration_ms_); }

/* Runs each generated timed case through the suite runner. */
#define PHYSICS_PRESET_RUN_CASE(id_, ...) RUN_TEST(test_physics_##id_);
