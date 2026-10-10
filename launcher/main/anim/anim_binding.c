#include "anim/anim_binding.h"

#include <stdio.h>
#include <string.h>

anim_bind_status_t
anim_bind_field(const anim_binding_t* binding, const anim_component_ref_t* components, int component_count,
                uint8_t* dirty, uint8_t dirty_bit, anim_bound_t* out) {
    for (int c = 0; c < component_count; c++) {
        const anim_component_fields_t* component = components[c].fields;
        if (component->component != binding->component) {
            continue;
        }
        for (int f = 0; f < component->field_count; f++) {
            const anim_field_t* field = &component->fields[f];
            if (strcmp(field->name, binding->field) != 0) {
                continue;
            }
            if (field->type != binding->type) {
                return ANIM_BIND_ERR_TYPE;
            }
            *out = (anim_bound_t){
                .curve = binding->curve,
                .target = (float*)(void*)((uint8_t*)components[c].base + field->offset),
                .dirty = dirty,
                .dirty_bit = dirty_bit,
            };
            return ANIM_BIND_OK;
        }
        return ANIM_BIND_ERR_FIELD;
    }
    return ANIM_BIND_ERR_COMPONENT;
}

anim_bind_status_t
anim_bind(const anim_tracks_t* clip, const anim_target_t* targets, int target_count, anim_bound_t* out, int out_count,
          int* failed) {
    if (failed != NULL) {
        *failed = -1;
    }
    if (clip->root != ANIM_ROOT_SCENE) {
        return ANIM_BIND_ERR_ROOT;
    }
    if (out_count < clip->count) {
        return ANIM_BIND_ERR_SPACE;
    }
    for (int i = 0; i < clip->count; i++) {
        anim_binding_t binding;
        (void)anim_tracks_binding_at(clip, i, &binding);
        const anim_target_t* target = NULL;
        for (int t = 0; t < target_count; t++) {
            if (strcmp(targets[t].name, binding.path) == 0) {
                target = &targets[t];
                break;
            }
        }
        const anim_bind_status_t status = target == NULL
                                              ? ANIM_BIND_ERR_PATH
                                              : anim_bind_field(&binding, target->components, target->component_count,
                                                                target->dirty, target->dirty_bit, &out[i]);
        if (status != ANIM_BIND_OK) {
            if (failed != NULL) {
                *failed = i;
            }
            return status;
        }
    }
    if (failed != NULL) {
        *failed = -1;
    }
    return ANIM_BIND_OK;
}

void
anim_apply(const anim_bound_t* bound, int count, float seconds) {
    for (int i = 0; i < count; i++) {
        const anim_bound_t* b = &bound[i];
        float value[ANIM_WIDTH_MAX];
        anim_track_sample(&b->curve, seconds, value);
        memcpy(b->target, value, b->curve.width * sizeof *value);
        if (b->dirty != NULL) {
            *b->dirty |= b->dirty_bit;
        }
    }
}

int
anim_binding_describe(const anim_binding_t* binding, char* out, size_t size) {
    enum { BYTE_BITS = 8, BYTE_MASK = 255 };

    return snprintf(out, size, "%s:%c%c%c%c.%s", binding->path, (int)(binding->component & BYTE_MASK),
                    (int)((binding->component >> BYTE_BITS) & BYTE_MASK),
                    (int)((binding->component >> (2 * BYTE_BITS)) & BYTE_MASK),
                    (int)((binding->component >> (3 * BYTE_BITS)) & BYTE_MASK), binding->field);
}
