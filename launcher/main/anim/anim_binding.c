#include "anim/anim_binding.h"

#include <stdio.h>
#include <string.h>

static anim_bind_status_t
locate_field(const anim_binding_t* binding, const anim_component_ref_t* components, int component_count,
             anim_binding_location_t* out) {
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
            out->component = c;
            out->field = f;
            return ANIM_BIND_OK;
        }
        return ANIM_BIND_ERR_FIELD;
    }
    return ANIM_BIND_ERR_COMPONENT;
}

static anim_bound_t
bind_field(const anim_binding_t* binding, const anim_component_ref_t* component, int field, uint8_t* dirty,
           uint8_t dirty_bit) {
    return (anim_bound_t){
        .curve = binding->curve,
        .target = (float*)(void*)((uint8_t*)component->base + component->fields->fields[field].offset),
        .dirty = dirty,
        .dirty_bit = dirty_bit,
    };
}

anim_bind_status_t
anim_bind_field(const anim_binding_t* binding, const anim_component_ref_t* components, int component_count,
                uint8_t* dirty, uint8_t dirty_bit, anim_bound_t* out) {
    anim_binding_location_t location;
    const anim_bind_status_t status = locate_field(binding, components, component_count, &location);
    if (status == ANIM_BIND_OK) {
        *out = bind_field(binding, &components[location.component], location.field, dirty, dirty_bit);
    }
    return status;
}

/* The self target occupies the first search slot even when a path is named. */
static int
find_target(const anim_target_t* targets, int count, int self, name_hash_t name) {
    const int first = self == ANIM_BIND_NO_SELF ? 0 : self;
    for (int t = 0; t < count; t++) {
        const int target = t == 0 ? first : t == first ? 0 : t;
        if (targets[target].name == name) {
            return target;
        }
    }
    return -1;
}

anim_bind_status_t
anim_bind_locate(const anim_binding_t* binding, const anim_target_t* targets, int target_count, int self,
                 anim_binding_location_t* out) {
    const int target = strcmp(binding->path, ANIM_TARGET_SELF) == 0
                           ? (target_count > 0 ? self : ANIM_BIND_NO_SELF)
                           : find_target(targets, target_count, self, binding->path_hash);
    if (target < 0) {
        return ANIM_BIND_ERR_PATH;
    }
    out->target = target;
    return locate_field(binding, targets[target].components, targets[target].component_count, out);
}

anim_bind_status_t
anim_bind(const anim_tracks_t* clip, const anim_target_t* targets, int target_count, int self, anim_bound_t* out,
          int out_count, int* failed) {
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
        anim_binding_location_t location;
        const anim_bind_status_t status = anim_bind_locate(&binding, targets, target_count, self, &location);
        if (status != ANIM_BIND_OK) {
            if (failed != NULL) {
                *failed = i;
            }
            return status;
        }
        const anim_target_t* target = &targets[location.target];
        out[i] = bind_field(&binding, &target->components[location.component], location.field, target->dirty,
                            target->dirty_bit);
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
