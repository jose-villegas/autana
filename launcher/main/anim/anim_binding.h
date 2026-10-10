/*
 * anim_binding: resolve a scene clip against component field declarations.
 * Resolved curves sample directly into their targets without name lookups.
 */
#pragma once
#include <stddef.h>
#include "anim/anim_tracks.h"

typedef struct {
    const char* name;
    anim_value_t type;
    uint16_t offset;
} anim_field_t;

typedef struct {
    uint32_t component;
    const anim_field_t* fields;
    uint8_t field_count;
} anim_component_fields_t;

typedef struct {
    const char* name;
    const anim_component_fields_t* const* components;
    void* const* bases;
    uint8_t component_count;
    uint8_t* dirty;
    uint8_t dirty_bit;
} anim_target_t;

typedef struct {
    anim_track_t curve;
    float* target;
    uint8_t* dirty;
    uint8_t dirty_bit;
    uint8_t type;
} anim_bound_t;

typedef enum {
    ANIM_BIND_OK,
    ANIM_BIND_ERR_ROOT,
    ANIM_BIND_ERR_PATH,
    ANIM_BIND_ERR_COMPONENT,
    ANIM_BIND_ERR_FIELD,
    ANIM_BIND_ERR_TYPE,
    ANIM_BIND_ERR_SPACE,
} anim_bind_status_t;

/* On failure, failed receives the binding index; root and space use index 0. */
anim_bind_status_t anim_bind(const anim_tracks_t* clip, const anim_target_t* targets, int target_count,
                             anim_bound_t* out, int out_count, int* failed);
void anim_apply(const anim_bound_t* bound, int first, int count, float seconds);
/* Returns the snprintf length of path:CCCC.field. */
int anim_binding_describe(const anim_binding_t* binding, char* out, size_t size);
