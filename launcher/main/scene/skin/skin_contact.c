/* skin_contact: bind-space leaf contacts for ground shadows. */
#include "scene/skin/skin_contact.h"

#include <float.h>
#include <math.h>
#include "math/linear/mat4f.h"

vec3f_t
skin_contact_bind_position(const r3d_skin_t* skin, int joint) {
    const mat4f_t bind = mat4f_invert_affine(r3d_skin_inverse_bind(skin, joint));
    return mat4f_apply(&bind, vec3f_zero());
}

static bool
leaf(const anim_skeleton_t* skeleton, int joint) {
    for (int j = 0; j < skeleton->joint_count; j++) {
        if (skeleton->joints[j].parent == joint) {
            return false;
        }
    }
    return true;
}

static bool
contact_heights(const anim_skeleton_t* skeleton, const r3d_skin_t* skin, float* height, float* lowest) {
    float low = FLT_MAX;
    float high = -FLT_MAX;
    float lowest_leaf = FLT_MAX;
    for (int j = 0; j < skin->joint_count; j++) {
        const vec3f_t p = skin_contact_bind_position(skin, j);
        if (!isfinite(p.x) || !isfinite(p.y) || !isfinite(p.z)) {
            return false;
        }
        low = fminf(low, p.y);
        high = fmaxf(high, p.y);
        if (leaf(skeleton, j)) {
            lowest_leaf = fminf(lowest_leaf, p.y);
        }
    }
    *height = high - low;
    *lowest = lowest_leaf;
    return true;
}

static void
contact_insert(skin_contacts_t* out, const r3d_skin_t* skin, int joint, float y) {
    int at = out->count;
    while (at > 0) {
        const float rest_y = skin_contact_bind_position(skin, out->joints[at - 1]).y;
        if (y >= rest_y) {
            break;
        }
        if (at < SKIN_CONTACTS_MAX) {
            out->joints[at] = out->joints[at - 1];
        }
        at--;
    }
    if (at < SKIN_CONTACTS_MAX) {
        out->joints[at] = joint;
    }
    if (out->count < SKIN_CONTACTS_MAX) {
        out->count++;
    }
}

static float
contact_spacing(const r3d_skin_t* skin, const skin_contacts_t* out) {
    float nearest = FLT_MAX;
    for (int i = 0; i < out->count; i++) {
        for (int j = i + 1; j < out->count; j++) {
            vec3f_t d = vec3f_sub(skin_contact_bind_position(skin, out->joints[i]),
                                  skin_contact_bind_position(skin, out->joints[j]));
            d.y = 0;
            const float spacing = vec3f_length(d);
            if (spacing > 0) {
                nearest = fminf(nearest, spacing);
            }
        }
    }
    return nearest;
}

int
skin_contacts_find(const anim_skeleton_t* skeleton, const r3d_skin_t* skin, skin_contacts_t* out) {
    *out = (skin_contacts_t){0};
    if (skeleton->joint_count != skin->joint_count || skin->joint_count == 0) {
        return 0;
    }
    float height;
    float lowest_leaf;
    if (!contact_heights(skeleton, skin, &height, &lowest_leaf)) {
        return 0;
    }
    for (int j = 0; j < skin->joint_count; j++) {
        const float y = skin_contact_bind_position(skin, j).y;
        if (leaf(skeleton, j) && y <= lowest_leaf + (SKIN_CONTACT_JOINT_BAND * height)) {
            contact_insert(out, skin, j, y);
        }
    }
    const float nearest = contact_spacing(skin, out);
    out->radius = SKIN_CONTACT_RADIUS_FRACTION * (nearest < FLT_MAX ? nearest : height);
    return out->count;
}
