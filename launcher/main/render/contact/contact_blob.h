/* contact_blob: one depth-tested, dithered ground footprint with radial or mask coverage. */
#pragma once
#include "render/contact/contact_mask.h"
#include "render/r3d_pipeline.h"

typedef struct {
    vec3f_t centre, u, v;
    float strength;
    const contact_mask_t* mask;
} contact_blob_t;

void contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* blob);
