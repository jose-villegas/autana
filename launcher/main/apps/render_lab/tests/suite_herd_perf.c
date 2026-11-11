/* Device suite: each herd size shows its first N capybaras and hides the
 * rest, every one shown drawn, and what a turn of the camera round them
 * costs: the whole frame, and the animation step (posing and skinning every
 * copy shown) alone, with the internal heap the herd leaves, the copies whose
 * skin buffers MEMORY_FAST placed in external RAM, and the copies in view on
 * average.
 * Logged per size and quarter, no budget yet; the slope over N is the cost of
 * one more skinned copy. */
#include "suites.h"
#ifdef DEVICE_BUILD
#include <math.h>

#include "apps/render_lab/herd/scene_herd.h"
#include "apps/render_lab/render_lab.h"
#include "core/memory.h"
#include "core/timing.h"
#include "core/tune.h"
#include "esp_log.h"
#include "frame/test_frame.h"
#include "gfx/gfx.h"
#include "lab/lab_view.h"
#include "lab/test/lab_perf_quarters.h"
#include "lab/test/lab_test.h"
#include "math/linear/transformf.h"
#include "scene/camera/camera.h"
#include "scene/entity/entity.h"
#include "scene/renderer/renderer.h"
#include "scene/scene_internal.h"
#include "scene/transform/transform.h"
#include "testkit/bench.h"
#include "testkit/cleanup.h"
#include "unity.h"

/* Frames sampled per turn of the camera, spread evenly round it. */
#define HERD_PERF_SAMPLES 100

/* One turn of the camera (render_lab.orbit_ms), and so each sample's step. */
#define HERD_PERF_TURN_MS 18000
#define HERD_PERF_DT_MS   (HERD_PERF_TURN_MS / HERD_PERF_SAMPLES)
_Static_assert(HERD_PERF_TURN_MS <= LAB_ORBIT_MAX_MS, "a turn render_lab.orbit_ms can hold");

static const char* TAG = "herd_perf";

typedef struct {
    lab_test_t saved;
    int32_t frame_us[HERD_PERF_SAMPLES];
    int32_t step_us[HERD_PERF_SAMPLES];
} fixture_t;

static fixture_t* active;
static const lab_scene_t* herd; /* the size the next test shows */

static void
release_fixture(void) {
    (void)tune_reset(tune_shared(), TUNE_NAME(render_lab, orbit_ms));
    lab_test_exit(&active->saved);
    memory_free(active);
    active = NULL;
}

/* The skinned copies the scene keeps active and, in `external`, those of
 * them whose skin buffers MEMORY_FAST put in external RAM. */
static int
shown_copies(const scene_t* scene, int* external) {
    int shown = 0;
    *external = 0;
    for (int e = 0; e < scene->asset.entity_count; e++) {
        const scene_skinned_renderer_t* skinned = &scene->skinned_renderers[e];
        if (skinned->bound && entity_active(scene_row_entity(scene, (scene_row_t)e))) {
            shown++;
            *external += memory_in_psram(skinned->storage);
        }
    }
    return shown;
}

/* Whether the sphere at `centre` (camera space: x right, y down, z ahead) is
 * on the inner side of the plane through the eye leaning out `tan_half` from
 * the view axis along `side`. */
static bool
inside_side(float side, float z, float tan_half, float radius) {
    return side - (tan_half * z) <= radius * sqrtf(1.0F + (tan_half * tan_half));
}

/* The skinned copies the scene keeps active whose bounding sphere meets the
 * active camera's view, drawn at `quarter`. */
static int
copies_in_view(const scene_t* scene, int quarter) {
    const entity_t camera = camera_active();
    const lens_t* lens = camera_lens(camera);
    const transformf_t pose = transform_world(camera);
    const bool turned = (quarter & 1) != 0; /* a quarter turn swaps the picture's sides */
    const float width = (float)(turned ? GFX_HEIGHT : GFX_WIDTH);
    const float height = (float)(turned ? GFX_WIDTH : GFX_HEIGHT);
    const float short_side = fminf(width, height);
    const float tan_x = lens->half_fov_short_tan * width / short_side;
    const float tan_y = lens->half_fov_short_tan * height / short_side;
    const vec3f_t right = quatf_rotate(pose.rotation, vec3f_unit_x());
    const vec3f_t down = quatf_rotate(pose.rotation, vec3f_down());
    const vec3f_t ahead = quatf_rotate(pose.rotation, vec3f_unit_z());
    int seen = 0;
    for (int e = 0; e < scene->asset.entity_count; e++) {
        const entity_t copy = scene_row_entity(scene, (scene_row_t)e);
        if (!scene->skinned_renderers[e].bound || !entity_active(copy)) {
            continue;
        }
        const spheref_t local = r3d_lit_mesh_bounding_sphere(renderer_mesh(copy));
        const transformf_t at = transform_world(copy);
        const vec3f_t to = vec3f_sub(vec3f_add(at.position, quatf_rotate(at.rotation, local.centre)), pose.position);
        const float x = vec3f_dot(to, right);
        const float y = vec3f_dot(to, down);
        const float z = vec3f_dot(to, ahead);
        seen += z + local.radius > lens->near_z && inside_side(x, z, tan_x, local.radius)
                && inside_side(-x, z, tan_x, local.radius) && inside_side(y, z, tan_y, local.radius)
                && inside_side(-y, z, tan_y, local.radius);
    }
    return seen;
}

static void
test_herd_turn_cost(void) {
    active = memory_alloc(sizeof *active, MEMORY_PSRAM);
    TEST_ASSERT_NOT_NULL(active);
    *active = (fixture_t){0};
    const size_t internal_before = memory_free_bytes(MEMORY_INTERNAL);
    lab_test_enter(&active->saved, &render_lab, herd->key);
    suite_set_test_cleanup(release_fixture);
    TEST_ASSERT_TRUE(tune_set(tune_shared(), TUNE_NAME(render_lab, orbit_ms), HERD_PERF_TURN_MS));
    const entity_t root = lab_test_loaded_scene("herd");
    TEST_ASSERT_FALSE(handle_is_none(root));
    scene_row_t row;
    scene_t* scene = scene_entity_row(herd->path().entity, &row); /* a row of a copy: the root is not one */
    TEST_ASSERT_NOT_NULL(scene);
    const int copies = *(const int*)herd->data;
    scene_animation_step(scene, 0); /* a shown copy takes its buffers on its first step */
    int external = 0;
    TEST_ASSERT_EQUAL_INT(copies, shown_copies(scene, &external));
    TEST_ASSERT_EQUAL_INT(0, scene->starved_count);
    ESP_LOGI(TAG, "herd_perf: n=%d internal_taken=%u internal_free=%u internal_largest=%u", copies,
             (unsigned)(internal_before - memory_free_bytes(MEMORY_INTERNAL)),
             (unsigned)memory_free_bytes(MEMORY_INTERNAL), (unsigned)memory_largest_block(MEMORY_INTERNAL));
    ESP_LOGI(TAG, "herd_perf: n=%d copies_in_external_ram=%d", copies, external);

    for (int i = -1; i < HERD_PERF_SAMPLES; i++) {
        const int64_t start = timing_now_us();
        scene_animation_step(scene, HERD_PERF_DT_MS);
        if (i >= 0) {
            active->step_us[i] = (int32_t)(timing_now_us() - start);
        }
    }
    const bench_stats_t step = bench_stats_compute(active->step_us, HERD_PERF_SAMPLES);
    TEST_ASSERT_GREATER_THAN_INT64(0, step.avg);
    ESP_LOGI(TAG, "herd_perf: n=%d samples=%d step_mean_us=%lld step_p95_us=%lld", copies, HERD_PERF_SAMPLES,
             (long long)step.avg, (long long)step.p95);

    for (size_t q = 0; q < LAB_PERF_QUARTER_COUNT; q++) {
        const lab_perf_quarter_t* quarter = &lab_perf_quarters[q];
        test_hold_quarter(quarter->quarter);
        int in_view = 0; /* summed over the samples, counted outside their timing */
        const vec3f_t eye = transform_world(camera_active()).position;
        bool turned = false;
        for (int i = -1; i < HERD_PERF_SAMPLES; i++) {
            const int64_t start = timing_now_us();
            (void)lab_test_frame_dt(HERD_PERF_DT_MS, false, NULL);
            if (i >= 0) {
                active->frame_us[i] = (int32_t)(timing_now_us() - start);
                in_view += copies_in_view(scene, quarter->quarter);
                turned |= vec3f_length(vec3f_sub(transform_world(camera_active()).position, eye)) > 0.0F;
            }
        }
        TEST_ASSERT_TRUE_MESSAGE(turned, "the camera turns about the herd");
        TEST_ASSERT_EQUAL_INT(0, scene->starved_count);
        const bench_stats_t frame = bench_stats_compute(active->frame_us, HERD_PERF_SAMPLES);
        TEST_ASSERT_GREATER_THAN_INT64(0, frame.avg);
        ESP_LOGI(
            TAG,
            "herd_perf: n=%d samples=%d frame_mean_us=%lld frame_p95_us=%lld in_view_mean=%.2f" LAB_PERF_QUARTER_FORMAT,
            copies, HERD_PERF_SAMPLES, (long long)frame.avg, (long long)frame.p95, (double)in_view / HERD_PERF_SAMPLES,
            quarter->name);
    }
    suite_clear_test_cleanup();
    release_fixture();
}
#endif

void
run_herd_perf_suite(void) {
#ifdef DEVICE_BUILD
    for (int i = 0; i < HERD_SIZE_COUNT; i++) {
        herd = &scene_herd[i];
        RUN_TEST_CASE(test_herd_turn_cost, herd->key);
    }
#endif
}

SUITE_REGISTER(run_herd_perf_suite);
