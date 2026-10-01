/*
 * perf_region, a named counter window for a stage of one frame. A window
 * reads cycles and one Xtensa event on one core. Regions do not nest: the
 * S3 has only the two counters this owns, so a second begin is ignored until
 * the matching end. Host and release brackets compile to nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define PERF_REGION_SLOTS          16
#define PERF_REGION_NAME_MAX       31
#define PERF_REGION_EVENT_NAME_MAX 31

typedef struct perf_region_entry {
    const char* name;
    struct perf_region_entry* next;
} perf_region_entry_t;

typedef struct {
    perf_region_entry_t* first;
    int count;
    int overflowed;
} perf_region_registry_t;

/* A duplicate entry is harmless; a table that is full rejects new names. */
static inline bool
perf_region_register(perf_region_registry_t* registry, perf_region_entry_t* entry) {
    for (perf_region_entry_t* found = registry->first; found != NULL; found = found->next) {
        if (found == entry) {
            return true;
        }
        if (strcmp(found->name, entry->name) == 0) {
            return false;
        }
    }
    if (registry->count >= PERF_REGION_SLOTS) {
        registry->overflowed++;
        return false;
    }
    entry->next = registry->first;
    registry->first = entry;
    registry->count++;
    return true;
}

static inline perf_region_entry_t*
perf_region_find(const perf_region_registry_t* registry, const char* name) {
    for (perf_region_entry_t* entry = registry->first; entry != NULL; entry = entry->next) {
        if (entry->name == name || strcmp(entry->name, name) == 0) {
            return entry;
        }
    }
    return NULL;
}

typedef struct {
    const char* name;
    uint16_t select;
    uint16_t mask;
} perf_region_event_t;

typedef struct {
    uint32_t cycles;
    uint32_t event;
    bool cycles_overflowed;
    bool event_overflowed;
} perf_region_counts_t;

typedef struct {
    const perf_region_entry_t* region;
    const perf_region_event_t* event;
    int core;
    bool active;
} perf_region_mark_t;

typedef void (*perf_region_reply_fn)(const char* line);

#if defined(ESP_PLATFORM) && CONFIG_LAUNCHER_DEVELOPMENT
#define PERF_REGION_ENABLED 1
#else
#define PERF_REGION_ENABLED 0
#endif

#if PERF_REGION_ENABLED
perf_region_registry_t* perf_region_shared(void);
const perf_region_event_t* perf_region_event_find(const char* name);
int perf_region_event_count(void);
const perf_region_event_t* perf_region_event_at(int index);
bool perf_region_begin(perf_region_mark_t* mark, const perf_region_entry_t* region, const perf_region_event_t* event);
bool perf_region_end(perf_region_mark_t* mark, perf_region_counts_t* counts);
bool perf_region_handle_line(const char* line, perf_region_reply_fn reply);

#define PERF_REGION(symbol, text)                                                                                      \
    static perf_region_entry_t symbol = {text, NULL};                                                                  \
    __attribute__((constructor)) static void symbol##_perf_region_register(void) {                                     \
        perf_region_register(perf_region_shared(), &(symbol));                                                         \
    }

#define PERF_REGION_BEGIN(mark, region)                                                                                \
    perf_region_mark_t mark = {0};                                                                                     \
    (void)perf_region_begin(&(mark), &(region), NULL)
#define PERF_REGION_END(mark) (void)perf_region_end(&(mark), NULL)
#else
#define PERF_REGION(symbol, text)
#define PERF_REGION_BEGIN(mark, region) ((void)0)
#define PERF_REGION_END(mark)           ((void)0)
#endif
