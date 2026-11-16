/* contact_blob: depth-tested dithered contact discs in a target's rows. */
#pragma once
#include "render/r3d_pipeline.h"

/* The lens maps world units, with the geometry pass's fitted depth scale. */
void contact_blob_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, vec3f_t ground, vec3f_t up,
                       float radius, float strength);
