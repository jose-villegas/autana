/* render_lab_test: Render Lab's own fixture beside the lab's (lab/test/lab_test.h). */
#pragma once
#ifdef DEVICE_BUILD
#include <stddef.h>
#include <stdint.h>
#include "lab/test/lab_test.h"
#include "render/view/raster_cull_view.h"
#include "scene/scene.h"

/* Ladder step `step`'s RUN_TEST_CASE segment, its size: "368x448". Valid
 * until the next call. */
const char* render_lab_test_step_name(int step);

/* Shows debug view `view` from the next draw on: the scene applies
 * render_lab.view after it draws, so the context is set here as well. */
void render_lab_test_show_view(int view);

/* The cull view's counts, one per RASTER_CULL_FRAME_FIELDS entry, and the
 * longest "name=value ..." line of them. */
#define RENDER_LAB_TEST_COUNT_ONE(name, member)   +1
#define RENDER_LAB_TEST_COUNT_BYTES(name, member) char name[sizeof(" " #name "=-9223372036854775808")];

enum { RENDER_LAB_TEST_COUNTS = 0 RASTER_CULL_FRAME_FIELDS(RENDER_LAB_TEST_COUNT_ONE) };

#define RENDER_LAB_TEST_COUNTS_MAX sizeof(struct { RASTER_CULL_FRAME_FIELDS(RENDER_LAB_TEST_COUNT_BYTES) char end; })

/* Adds the counts of the frame the cull view just drew to `sums`. */
void render_lab_test_add_cull_counts(int64_t sums[RENDER_LAB_TEST_COUNTS]);

/* Writes `sums` over `frames` frames, each its rounded mean, as one count
 * line into `out`. */
void render_lab_test_write_counts(const int64_t sums[RENDER_LAB_TEST_COUNTS], int frames, char* out, size_t size);

/* Draws the pose `path_ms` along `entity`'s current clip of `duration_ms`,
 * paused there (dt 0): one frame through the cull view for its counts, then
 * FRAME_PROFILE_REPLAY_DRAWS shaded frames for their frame-cost report, and
 * logs both as one FRAME_PROFILE_REPLAY line under `tag`. */
void render_lab_test_replay_pose(const char* tag, entity_t entity, uint32_t duration_ms, int32_t path_ms,
                                 const char* row, const char* quarter);

#endif
