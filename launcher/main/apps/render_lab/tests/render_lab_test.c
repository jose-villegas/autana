#include "apps/render_lab/tests/render_lab_test.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include "apps/render_lab/render_lab_dynres.h"
#include "apps/render_lab/render_lab_view.h"
#include "collections/strbuf.h"
#include "core/memory.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "frame_profile.h"
#include "lab/test/lab_perf_quarters.h"
#include "profile/frame_cost.h"
#include "render/context/render_context.h"
#include "render/view/raster_cull_view.h"
#include "scene/animator/animator.h"
#include "unity.h"

const char*
render_lab_test_step_name(int step) {
    static char name[sizeof "-2147483648x-2147483648"];
    const resolution_step_t size = render_lab_ladder[step];
    (void)snprintf(name, sizeof name, "%dx%d", size.width, size.height);
    return name;
}

void
render_lab_test_show_view(int view) {
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, view), view));
    render_context_set_debug_view(render_context_main(), view);
    TEST_ASSERT_EQUAL_INT(view, render_context_main()->debug_view);
}

/* A replayed pose's log line parts, too big for the stack its draws run on. */
typedef struct {
    raster_cull_frame_t frame;
    int64_t sums[RENDER_LAB_TEST_COUNTS];
    char counts[RENDER_LAB_TEST_COUNTS_MAX];
    char report[FRAME_COST_REPORT_MAX];
} pose_line_t;

static pose_line_t* pose_line;

static void
add_frame_counts(int64_t sums[RENDER_LAB_TEST_COUNTS], raster_cull_frame_t* f) {
    const raster_t* raster = &render_context_main()->raster;
    const resolution_step_t output = gfx_half_picture() != NULL
                                         ? raster_compose_size(raster)
                                         : (resolution_step_t){raster->destination_width, raster->destination_height};
    /* The counted overlay composes full size; stage times describe the shaded compose. */
    raster_cull_frame_compose(f, raster, output);
    TEST_ASSERT_GREATER_THAN_INT_MESSAGE(0, f->counts.nodes_visited, "the cull view did not draw");
    int i = 0;
#define ADD_COUNT(name, member) sums[i++] += f->member;
    RASTER_CULL_FRAME_FIELDS(ADD_COUNT)
}

/* In a frame of its own that is gone before the next draw. */
__attribute__((noinline)) void
render_lab_test_add_cull_counts(int64_t sums[RENDER_LAB_TEST_COUNTS]) {
    const raster_cull_t* cull = render_context_debug_view_state(render_context_main());
    TEST_ASSERT_NOT_NULL(cull);
    raster_cull_frame_t frame = raster_cull_frame(cull);
    add_frame_counts(sums, &frame);
}

void
render_lab_test_write_counts(const int64_t sums[RENDER_LAB_TEST_COUNTS], int frames, char* out, size_t size) {
    TEST_ASSERT_GREATER_THAN_INT(0, frames);
    strbuf_t line = strbuf_make(out, size);
    int i = 0;
#define COUNT_PAIR(name, member)                                                                                       \
    strbuf_appendf(&line, "%s" #name "=%lld", line.length == 0 ? "" : " ",                                             \
                   (long long)((sums[i++] + (frames / 2)) / frames));
    RASTER_CULL_FRAME_FIELDS(COUNT_PAIR)
}

void
render_lab_test_replay_pose(const char* tag, entity_t entity, uint32_t duration_ms, int32_t path_ms, const char* row,
                            const char* quarter) {
    /* Static, so a failed assertion's jump leaves it for the next pose to reuse. */
    if (pose_line == NULL) {
        pose_line = memory_alloc(sizeof *pose_line, MEMORY_INTERNAL);
    }
    TEST_ASSERT_NOT_NULL(pose_line);
    pose_line_t* out = pose_line;
    TEST_ASSERT_TRUE(animator_seek(entity, ANIMATOR_CURRENT, (uint32_t)path_ms % duration_ms));
    render_lab_test_show_view(render_context_debug_view_named("cull"));
    (void)lab_test_frame_dt(0, false, NULL);
    memset(out->sums, 0, sizeof out->sums);
    const raster_cull_t* cull = render_context_debug_view_state(render_context_main());
    TEST_ASSERT_NOT_NULL(cull);
    out->frame = raster_cull_frame(cull);
    render_lab_test_show_view(RENDER_DEBUG_VIEW_SHADED);
    (void)lab_test_frame_dt(0, false, NULL);
    frame_cost_take_clear(); /* forget whatever ran before */
    for (int i = 0; i < FRAME_PROFILE_REPLAY_DRAWS; i++) {
        (void)lab_test_frame_dt(0, false, NULL);
    }
    (void)frame_cost_take_report(FRAME_PROFILE_REPLAY_DRAWS, out->report, sizeof out->report);
    add_frame_counts(out->sums, &out->frame);
    render_lab_test_write_counts(out->sums, 1, out->counts, sizeof out->counts);
    ESP_LOGI(tag, FRAME_PROFILE_REPLAY LAB_PERF_QUARTER_FORMAT, row, (int)path_ms, out->counts, out->report, quarter);
    memory_free(pose_line);
    pose_line = NULL;
}

#endif
