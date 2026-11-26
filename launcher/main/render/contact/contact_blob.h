/* contact_blob: one depth-tested, dithered ground footprint with radial or mask coverage. */
#pragma once
#include "render/contact/contact_mask.h"
#include "render/r3d_pipeline.h"

#define CONTACT_BLOBS_MAX 8

typedef struct {
    vec3f_t centre, u, v;
    float strength;
    const contact_mask_t* mask;
} contact_blob_t;

/* Caller-owned workspace, aligned as malloc provides; reusable after each draw. */
size_t contact_blobs_work_bytes(void);
contact_blob_t* contact_blobs_work_footprints(void* scratch);

/* Uses geometry's lens and fitted depth scale. At most CONTACT_BLOBS_MAX coplanar,
 * equal-strength footprints; overlaps darken once. Pass a mask for every footprint, or pass all NULL for radial falloff. */
void contact_blobs_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* blobs, int count,
                        void* scratch);
