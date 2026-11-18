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

void contact_disc_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground, vec3f_t up,
                       float radius, float strength);

void contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* blob);

void contact_blobs_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* blobs,
                        int count);
