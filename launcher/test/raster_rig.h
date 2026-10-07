/*
 * raster_rig: quads drawn by a raster with further attachments, allocated
 * for one test and released after it, for suites that test what the raster
 * writes beside colour and depth.
 */
#pragma once

#include <stdbool.h>
#include <stdlib.h>

#include "r3d_quad_mesh.h"
#include "render/r3d.h"
#include "render/r3d_pipeline.h"
#include "suites.h"
#include "unity.h"

#define RASTER_RIG_QUADS 2

typedef struct {
    r3d_quad_t quad[RASTER_RIG_QUADS];
    r3d_placement_t placement[RASTER_RIG_QUADS]; /* each placed quad's, set before a draw */
    r3d_instance_t instance[RASTER_RIG_QUADS];
    raster_t raster;
} raster_rig_t;

static raster_rig_t* raster_rig_now;

static void
raster_rig_release(void) {
    if (raster_rig_now != NULL) {
        free(raster_rig_now->raster.scratch);
        free(raster_rig_now);
        raster_rig_now = NULL;
    }
}

/* `count` quads at `positions`, drawn in that order, quad i placed by
 * placement[i] when `placed` has bit i; `attachments` attached; scratch for
 * a picture up to `width` by `height`, the size it draws at until changed. */
static inline raster_rig_t*
raster_rig_open(const int16_t (*const positions[])[3], int count, unsigned placed,
                const raster_attachment_t* const* attachments, int attachment_count, int width, int height) {
    raster_rig_t* r = calloc(1, sizeof(*r));
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(count <= RASTER_RIG_QUADS);
    raster_rig_now = r;
    suite_set_test_cleanup(raster_rig_release);
    for (int i = 0; i < count; i++) {
        r3d_quad_init(&r->quad[i], positions[i]);
        r->instance[i] = (r3d_instance_t){&r->quad[i].mesh, (placed & (1U << i)) != 0 ? &r->placement[i] : NULL};
    }
    r->raster = (raster_t){.instances = r->instance,
                           .instance_count = count,
                           .width = width,
                           .height = height,
                           .clear = 0x1234,
                           .attachments = attachments,
                           .attachment_count = attachment_count};
    r->raster.scratch = malloc(raster_scratch_bytes(&r->raster));
    TEST_ASSERT_NOT_NULL(r->raster.scratch);
    return r;
}

/* Further attachment `k`'s pixels, every row, as the last draw left them. */
static inline const void*
raster_rig_attachment(const raster_rig_t* r, int k) {
    return r3d_pipeline_carve(&r->raster).picture.attachment[GFX_ATTACHMENT_FURTHER + k].pixels;
}
