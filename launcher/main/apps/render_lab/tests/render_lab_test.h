/* render_lab_test: app fixtures and the composed, presented frame for device suites. */
#pragma once
#ifdef DEVICE_BUILD
#include <stdbool.h>
#include <stdint.h>
#include "app/app.h"
#include "app/app_console.h"
#include "profile/frame_cost.h"
#include "render/view/raster_cull_view.h"
#include "scene/scene.h"

#define RENDER_LAB_TEST_FRAME_MS 16

typedef struct {
    const char* scene;
    const char* camera;
    bool hud;
} render_lab_test_t;

extern app_t app_render_lab;
entity_t render_lab_test_loaded_scene(const char* key);
void render_lab_test_enter(render_lab_test_t* saved, const char* scene);
void render_lab_test_exit(const render_lab_test_t* saved);
int64_t render_lab_test_frame_dt(uint32_t dt_ms, bool switching, const input_t* input);
/* Ladder step `step`'s RUN_TEST_CASE segment, its size: "368x448". Valid
 * until the next call. */
const char* render_lab_test_step_name(int step);

/* Shows debug view `view` from the next draw on: the scene applies
 * render_lab.view after it draws, so the context is set here as well. */
void render_lab_test_show_view(int view);

/* A replayed pose's log line parts, too big for the stack its draws run on. */
#define RENDER_LAB_TEST_COUNT_BYTES(name, member) char name[sizeof(" " #name "=-2147483648")];

typedef struct {
    char counts[sizeof(struct { RASTER_CULL_FRAME_FIELDS(RENDER_LAB_TEST_COUNT_BYTES) char end; })];
    char report[FRAME_COST_REPORT_MAX];
} render_lab_test_pose_t;

/* Draws the pose `path_ms` along `entity`'s current clip of `duration_ms`,
 * paused there (dt 0): one frame through the cull view for its counts, then
 * FRAME_PROFILE_REPLAY_DRAWS shaded frames for their frame-cost report, and
 * logs both as one FRAME_PROFILE_REPLAY line under `tag`. */
void render_lab_test_replay_pose(const char* tag, entity_t entity, uint32_t duration_ms, int32_t path_ms,
                                 const char* row, const char* quarter, render_lab_test_pose_t* out);

/* Inline, so it adds no stack frame to the deepest test chains. */
static inline int64_t
render_lab_test_frame(bool switching, const input_t* input) {
    return render_lab_test_frame_dt(RENDER_LAB_TEST_FRAME_MS, switching, input);
}
#endif
