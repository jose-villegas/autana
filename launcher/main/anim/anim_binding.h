/*
 * anim_binding: resolve a scene clip against component field declarations.
 * Resolved curves sample directly into their targets without name lookups.
 */
#pragma once

#define ANIM_TARGET_SELF "."
#include <stddef.h>
#include "anim/anim_tracks.h"

typedef struct {
    const char* name;
    anim_value_t type;
    uint16_t offset;
    uint8_t flags;
} anim_field_t;

enum { ANIM_FIELD_FLAG_STEP = 1U << 0, ANIM_FIELD_FLAG_ALLOW_NONE = 1U << 1 };

/* One declared field of struct `type`: its name is the member's own, so the
 * two cannot drift. */

#define ANIM_FIELD(type, member, value_type)      {#member, (value_type), offsetof(type, member), 0}
#define ANIM_FIELD_STEP(type, member, value_type) {#member, (value_type), offsetof(type, member), ANIM_FIELD_FLAG_STEP}
#define ANIM_FIELD_STEP_OPTIONAL_NAME(type, member, value_type)                                                        \
    {#member, (value_type), offsetof(type, member), ANIM_FIELD_FLAG_STEP | ANIM_FIELD_FLAG_ALLOW_NONE}

typedef struct {
    uint32_t component;
    const anim_field_t* fields;
    uint8_t field_count;
} anim_component_fields_t;

typedef struct {
    const anim_component_fields_t* fields;
    void* base;
} anim_component_ref_t;

typedef struct {
    name_hash_t name;
    const anim_component_ref_t* components;
    uint8_t component_count;
    uint8_t* dirty;
    uint8_t dirty_bit;
} anim_target_t;

typedef struct {
    anim_track_t curve;
    float* target;
    uint8_t* dirty;
    uint8_t dirty_bit;
} anim_bound_t;

typedef enum {
    ANIM_BIND_OK,
    ANIM_BIND_ERR_ROOT,
    ANIM_BIND_ERR_PATH,
    ANIM_BIND_ERR_COMPONENT,
    ANIM_BIND_ERR_FIELD,
    ANIM_BIND_ERR_TYPE,
    ANIM_BIND_ERR_SPACE,
    ANIM_BIND_ERR_INTERP,
} anim_bind_status_t;

typedef struct {
    int target, component, field;
} anim_binding_location_t;

enum { ANIM_BIND_NO_SELF = -1 };

/* self is the index of the target for ANIM_TARGET_SELF, or ANIM_BIND_NO_SELF. */
anim_bind_status_t anim_bind_locate(const anim_binding_t* binding, const anim_target_t* targets, int target_count,
                                    int self, anim_binding_location_t* out);
/* Finds the component and field among components and checks the type. */
anim_bind_status_t anim_bind_field(const anim_binding_t* binding, const anim_component_ref_t* components,
                                   int component_count, uint8_t* dirty, uint8_t dirty_bit, anim_bound_t* out);
/* failed is a failed binding's index, or -1 on success and root or space errors. */
anim_bind_status_t anim_bind(const anim_tracks_t* clip, const anim_target_t* targets, int target_count, int self,
                             anim_bound_t* out, int out_count, int* failed);
void anim_apply(const anim_bound_t* bound, int count, float seconds);
/* Returns the snprintf length of path:CCCC.field. */
int anim_binding_describe(const anim_binding_t* binding, char* out, size_t size);
