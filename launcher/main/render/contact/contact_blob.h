/* contact_blob: depth-tested dithered contact masks and discs in target rows. */
#pragma once
#include "render/contact/contact_mask.h"
#include "render/r3d_pipeline.h"
#define CONTACT_BLOB_DISCS_MAX 8

typedef struct {
    vec3f_t centre, axis;
    float inverse_long2, inverse_short2, strength;
} contact_disc_t;

/* Serial post pass; mask and discs share a plane, axes and up are unit vectors. */
void contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t up, const contact_disc_t* discs,
                       int count, const contact_mask_t* mask);
