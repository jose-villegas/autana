/* contact_mask: top-down posed-mesh coverage on a ground rectangle. */
#pragma once
#include "render/r3d_lit_mesh.h"
#define CONTACT_MASK_SIDE  32
#define CONTACT_MASK_BYTES (CONTACT_MASK_SIDE * CONTACT_MASK_SIDE)

typedef struct {
    uint8_t* coverage;
    int side;
    vec3f_t origin, u, v;
} contact_mask_t;

/* Serial pose refit; coverage holds CONTACT_MASK_BYTES caller-owned bytes. */
void contact_mask_build(contact_mask_t* mask, const r3d_lit_mesh_t* mesh, const mat4f_t* world, box3f_t mover,
                        float ground_y);
