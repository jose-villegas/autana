/* contact_mask: weighted bind-space joint ellipsoids projected into a ground mask. */
#include "render/contact/contact_mask.h"

#include <math.h>
#include <string.h>
#include "math/linear/mat3f.h"

static void
bind_accumulate(contact_joint_t* joints, const r3d_lit_mesh_t* bind, const r3d_skin_t* skin, bool variance) {
    for (int v = 0; v < bind->vertex_count; v++) {
        const uint8_t* record = skin->vertices + ((size_t)v * skin->vertex_stride);
        const vec3f_t p = {(float)bind->positions[v][0], (float)bind->positions[v][1], (float)bind->positions[v][2]};
        for (int i = 0; i < skin->influences; i++) {
            contact_joint_t* joint = &joints[record[i]];
            const float w = r3d_skin_weight(record, skin->influences, i);
            if (variance) {
                const vec3f_t d = vec3f_sub(p, joint->centre);
                joint->radius = vec3f_add(joint->radius, vec3f_scale((vec3f_t){d.x * d.x, d.y * d.y, d.z * d.z}, w));
            } else {
                joint->weight += w;
                joint->centre = vec3f_add(joint->centre, vec3f_scale(p, w));
            }
        }
    }
}

void
contact_mask_bind(contact_joint_t* joints, const r3d_lit_mesh_t* bind, const r3d_skin_t* skin) {
    memset(joints, 0, (size_t)skin->joint_count * sizeof *joints);
    bind_accumulate(joints, bind, skin, false);
    for (int j = 0; j < skin->joint_count; j++) {
        if (joints[j].weight > 0) {
            joints[j].centre = vec3f_scale(joints[j].centre, 1.0F / joints[j].weight);
        }
    }
    bind_accumulate(joints, bind, skin, true);
    for (int j = 0; j < skin->joint_count; j++) {
        contact_joint_t* joint = &joints[j];
        if (joint->weight > 0) {
            joint->radius = (vec3f_t){CONTACT_MASK_SPREAD * sqrtf(joint->radius.x / joint->weight),
                                      CONTACT_MASK_SPREAD * sqrtf(joint->radius.y / joint->weight),
                                      CONTACT_MASK_SPREAD * sqrtf(joint->radius.z / joint->weight)};
        }
    }
}

static void
mask_ellipse(contact_mask_t* mask, vec2f_t centre, float xx, float xz, float zz) {
    /* A half-cell floor keeps flat or line-like joint bounds sampleable. */
    const float floor_variance = CONTACT_MASK_CELL_HALF * CONTACT_MASK_CELL_HALF;
    xx += floor_variance;
    zz += floor_variance;
    const float rx = sqrtf(xx);
    const float rz = sqrtf(zz);
    const float inverse = 1.0F / ((xx * zz) - (xz * xz));
    const float edge = fminf(rx, rz);
    const int left = mathi_max(0, (int)floorf(centre.x - rx));
    const int right = mathi_min(CONTACT_MASK_SIDE, (int)ceilf(centre.x + rx));
    const int top = mathi_max(0, (int)floorf(centre.y - rz));
    const int bottom = mathi_min(CONTACT_MASK_SIDE, (int)ceilf(centre.y + rz));
    const float quadratic = zz * inverse;
    const float second_difference = 2.0F * quadratic;
    for (int y = top; y < bottom; y++) {
        const float z = (float)y + CONTACT_MASK_CELL_HALF - centre.y;
        const float dx = (float)left + CONTACT_MASK_CELL_HALF - centre.x;
        const float cross = 2.0F * xz * z * inverse;
        float d2 = (quadratic * dx * dx) - (cross * dx) + (xx * z * z * inverse);
        float step = (quadratic * ((2.0F * dx) + 1.0F)) - cross;
        for (int x = left; x < right; x++, d2 += step, step += second_difference) {
            uint8_t* cell = &mask->coverage[(y * CONTACT_MASK_SIDE) + x];
            if (*cell == UINT8_MAX || d2 >= 1.0F) {
                continue;
            }
            const float coverage = (1.0F - d2) * edge;
            const uint8_t value = coverage >= 1.0F ? UINT8_MAX : (uint8_t)(coverage * UINT8_MAX);
            *cell = *cell > value ? *cell : value;
        }
    }
}

static void
mask_joint(contact_mask_t* mask, const contact_joint_t* joint, const mat4f_t* palette, int scale, const mat4f_t* world,
           box3f_t mover) {
    const mat3f_t linear = mat3f_mul(mat3f_from_mat4(*world), mat3f_from_mat4(*palette));
    const float reciprocal = 1.0F / (float)scale;
    const vec3f_t local = vec3f_scale(mat4f_apply(palette, joint->centre), reciprocal);
    const vec3f_t p = mat4f_apply(world, local);
    const float sx = (float)CONTACT_MASK_SIDE / (mover.hi.x - mover.lo.x);
    const float sz = (float)CONTACT_MASK_SIDE / (mover.hi.z - mover.lo.z);
    const vec2f_t centre = {(p.x - mover.lo.x) * sx, (p.z - mover.lo.z) * sz};
    const vec3f_t x = {linear.m[0][0] * joint->radius.x * sx * reciprocal,
                       linear.m[0][1] * joint->radius.y * sx * reciprocal,
                       linear.m[0][2] * joint->radius.z * sx * reciprocal};
    const vec3f_t z = {linear.m[2][0] * joint->radius.x * sz * reciprocal,
                       linear.m[2][1] * joint->radius.y * sz * reciprocal,
                       linear.m[2][2] * joint->radius.z * sz * reciprocal};
    mask_ellipse(mask, centre, vec3f_dot(x, x), vec3f_dot(x, z), vec3f_dot(z, z));
}

void
contact_mask_build(contact_mask_t* mask, const contact_joint_t* joints, int count, const mat4f_t* palette,
                   int position_scale, const mat4f_t* world, box3f_t mover) {
    memset(mask->coverage, 0, CONTACT_MASK_BYTES);
    if (!(mover.hi.x > mover.lo.x && mover.hi.z > mover.lo.z)) {
        return;
    }
    for (int j = 0; j < count; j++) {
        if (joints[j].weight > 0) {
            mask_joint(mask, &joints[j], &palette[j], position_scale, world, mover);
        }
    }
}
