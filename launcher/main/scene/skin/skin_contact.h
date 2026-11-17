/* skin_contact: bind-space leaf contacts for ground shadows. */
#pragma once
#include "anim/anim_skeleton.h"
#include "math/linear/vec3f.h"
#include "render/contact/contact_blob.h"
#include "render/r3d_skin.h"
#define SKIN_CONTACTS_MAX       CONTACT_BLOBS_MAX
#define CONTACT_JOINT_BAND      0.1F
#define CONTACT_RADIUS_FRACTION 0.5F

typedef struct {
    uint8_t joints[SKIN_CONTACTS_MAX];
    float rest_y[SKIN_CONTACTS_MAX];
    float radius;
    uint8_t count;
} skin_contacts_t;

int skin_contacts_find(const anim_skeleton_t* skeleton, const r3d_skin_t* skin, skin_contacts_t* out);

vec3f_t skin_contact_bind_position(const r3d_skin_t* skin, int joint);
