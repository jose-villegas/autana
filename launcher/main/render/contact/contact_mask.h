/* contact_mask: joint ellipsoid coverage on a ground rectangle. */
#pragma once
#include "render/r3d_lit_mesh.h"
#include "render/r3d_skin.h"
#define CONTACT_MASK_SIDE      32
#define CONTACT_MASK_BYTES     (CONTACT_MASK_SIDE * CONTACT_MASK_SIDE)

/* Two weighted standard deviations bound the proxy, not the mesh silhouette. */
#define CONTACT_MASK_SPREAD    2.0F
#define CONTACT_MASK_CELL_HALF 0.5F

typedef struct {
    vec3f_t centre, radius;
    float weight;
} contact_joint_t;

typedef struct {
    uint8_t* coverage;
} contact_mask_t;

/* Caller supplies one entry per joint, retained with the skin's buffers. */
void contact_mask_bind(contact_joint_t* joints, const r3d_lit_mesh_t* bind, const r3d_skin_t* skin);
void contact_mask_build(contact_mask_t* mask, const contact_joint_t* joints, int count, const mat4f_t* palette,
                        int position_scale, const mat4f_t* world, box3f_t mover);
