/*
 * entity: the world's entity table. A slot's generation lives here for the
 * whole run, so a handle kept past its scene's release stays refused even
 * when the slot is issued again. A scene holds two kinds of slot: its root,
 * issued at the request, and one run for its rows, issued when it opens.
 */
#include "scene/entity/entity.h"

#include <assert.h>
#include <string.h>

#include "esp_log.h"

#include "scene/scene_internal.h"
#include "scene/scene_shell.h"

#define WORLD_SLOTS (HANDLE_SLOT_MAX + 1U)

scene_root_t scene_roots[SCENE_ROOTS_MAX];

/* Each slot's latest generation; 0: never issued, HANDLE_GENERATION_MAX: retired once released. */
static uint16_t* generations;

static bool
entity_alive(handle_t owner) {
    scene_row_t row;
    return scene_entity_peek(owner, &row) != NULL;
}

bool
scene_world_init(void) {
    if (generations == NULL) {
        generations = memory_alloc(sizeof *generations * WORLD_SLOTS, MEMORY_PSRAM);
        if (generations == NULL) {
            return false;
        }
        memset(generations, 0, sizeof *generations * WORLD_SLOTS);
        pending_owner_register(HANDLE_OWNER_ENTITY, entity_alive);
    }
    return true;
}

static bool
holds(const scene_root_t* record, uint32_t slot) {
    return !handle_is_none(record->root)
           && (handle_slot(record->root) == slot
               || (slot >= record->base && slot < (uint32_t)record->base + record->rows));
}

static bool
slot_free(uint32_t slot) {
    if (!handle_generation_next(generations[slot])) {
        return false;
    }
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        if (holds(&scene_roots[i], slot)) {
            return false;
        }
    }
    return true;
}

static uint32_t
issue(uint32_t slot) {
    return ++generations[slot];
}

bool
scene_world_issue_root(scene_root_t* record) {
    if (!scene_world_init()) {
        return false;
    }
    for (uint32_t slot = 0; slot < WORLD_SLOTS; slot++) {
        if (slot_free(slot)) {
            record->root = handle_make(HANDLE_OWNER_ENTITY, slot, issue(slot));
            return true;
        }
    }
    return false;
}

bool
scene_world_issue_rows(scene_root_t* record, int rows) {
    uint32_t run = 0;
    for (uint32_t slot = 0; slot < WORLD_SLOTS && rows > 0; slot++) {
        run = slot_free(slot) ? run + 1U : 0U;
        if (run == (uint32_t)rows) {
            record->base = (uint16_t)(slot + 1U - run);
            for (uint32_t s = record->base; s <= slot; s++) {
                (void)issue(s);
            }
            record->rows = (uint16_t)rows;
            return true;
        }
    }
    return rows == 0;
}

scene_root_t*
scene_entity_peek(entity_t entity, scene_row_t* row) {
    if (handle_owner(entity) != HANDLE_OWNER_ENTITY || generations == NULL) {
        return NULL;
    }
    const uint32_t slot = handle_slot(entity);
    if (generations[slot] != handle_generation(entity)) {
        return NULL;
    }
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_root_t* record = &scene_roots[i];
        if (holds(record, slot)) {
            *row = handle_slot(record->root) == slot ? SCENE_ROW_ROOT : (scene_row_t)(slot - record->base);
            return record;
        }
    }
    return NULL;
}

scene_root_t*
scene_entity_lookup(entity_t entity, scene_row_t* row) {
    scene_root_t* record = scene_entity_peek(entity, row);
#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
    if (record == NULL && !handle_is_none(entity)) {
        ESP_LOGE("entity", "refused entity %08lx: stale or not an entity", (unsigned long)entity.id);
        assert(!"stale or foreign entity handle");
    }
#endif
    return record;
}

scene_t*
scene_entity_row(entity_t entity, scene_row_t* row) {
    scene_root_t* record = scene_entity_lookup(entity, row);
    return record == NULL || *row == SCENE_ROW_ROOT ? NULL : record->scene;
}

entity_t
scene_row_entity(const scene_t* scene, scene_row_t row) {
    if (row == SCENE_ROW_NONE) {
        return ENTITY_NONE;
    }
    if (row == SCENE_ROW_ROOT) {
        return scene->root->root;
    }
    const uint32_t slot = (uint32_t)scene->root->base + row;
    return handle_make(HANDLE_OWNER_ENTITY, slot, generations[slot]);
}

/* A READY scene's rows, for the walks below. */
static scene_t*
ready(const scene_root_t* record) {
    return record->state == PENDING_READY ? record->scene : NULL;
}

scene_t*
scene_ready_at(int index) {
    return ready(&scene_roots[index]);
}

static bool
row_within(const scene_t* scene, scene_row_t row, scene_row_t ancestor) {
    if (ancestor == SCENE_ROW_ROOT) {
        return true;
    }
    for (scene_row_t e = row; e != SCENE_ROW_NONE && e >= ancestor; e = scene_row_parent(scene, e)) {
        if (e == ancestor) {
            return true;
        }
    }
    return false;
}

/* The first entity named `name` below `top` in one READY scene. */
static entity_t
find_in(const scene_root_t* record, scene_row_t top, name_hash_t name) {
    const scene_t* scene = record->scene;
    for (int r = top == SCENE_ROW_ROOT ? 0 : top + 1; r < scene->asset.entity_count; r++) {
        if (scene->asset.names[r].hash == name && row_within(scene, (scene_row_t)r, top)) {
            return scene_row_entity(scene, (scene_row_t)r);
        }
    }
    return ENTITY_NONE;
}

entity_t
entity_find(entity_t within, name_hash_t name) {
    scene_row_t top;
    const scene_root_t* record = scene_entity_lookup(within, &top);
    return record == NULL || ready(record) == NULL ? ENTITY_NONE : find_in(record, top, name);
}

entity_t
entity_next(entity_t within, entity_t after) {
    scene_row_t top;
    scene_row_t from;
    const scene_root_t* record = scene_entity_lookup(within, &top);
    const scene_t* scene = record == NULL ? NULL : ready(record);
    if (scene == NULL) {
        return ENTITY_NONE;
    }
    int r = top == SCENE_ROW_ROOT ? 0 : top + 1;
    if (!handle_is_none(after)) {
        if (scene_entity_lookup(after, &from) != record || from == SCENE_ROW_ROOT) {
            return ENTITY_NONE;
        }
        r = from + 1;
    }
    for (; r < scene->asset.entity_count; r++) {
        if (row_within(scene, (scene_row_t)r, top)) {
            return scene_row_entity(scene, (scene_row_t)r);
        }
    }
    return ENTITY_NONE;
}

const char*
entity_name(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    if (record == NULL) {
        return NULL;
    }
    return row == SCENE_ROW_ROOT ? record->id : record->scene->asset.names[row].name;
}

bool
entity_enabled(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    return record != NULL && (scene_row_flags(record, row) & SCENE_FLAG_ENABLED) != 0;
}

void
entity_set_enabled(entity_t entity, bool enabled) {
    scene_row_t row;
    scene_root_t* record = scene_entity_lookup(entity, &row);
    if (record != NULL) {
        scene_row_set_enabled(record, row, enabled);
    }
}

bool
entity_active(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    return record != NULL && (scene_row_flags(record, row) & SCENE_FLAG_ACTIVE) != 0;
}

entity_t
entity_parent(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    if (record == NULL || row == SCENE_ROW_ROOT) {
        return ENTITY_NONE;
    }
    const scene_row_t parent = scene_row_parent(record->scene, row);
    return parent == SCENE_ROW_NONE ? record->root : scene_row_entity(record->scene, parent);
}

bool
entity_is_within(entity_t entity, entity_t ancestor) {
    scene_row_t row;
    scene_row_t top;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    if (record == NULL || scene_entity_lookup(ancestor, &top) != record) {
        return false;
    }
    if (row == SCENE_ROW_ROOT) {
        return top == SCENE_ROW_ROOT;
    }
    return row_within(record->scene, row, top);
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
scene_world_checkpoint_capture(checkpoint_t* saved) {
    CHECKPOINT_CAPTURE(saved, scene_roots);
    checkpoint_capture_at(saved, &generations, generations == NULL ? 0 : sizeof *generations * WORLD_SLOTS);
}
#endif
