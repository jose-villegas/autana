/* registry: shared suite registration and name-ordered access. */
#include "testkit/suite.h"

#include <stdbool.h>
#include <string.h>

static suite_entry_t* head;
static size_t suite_count;
static int dropped;

void
suite_link(suite_entry_t* entry) {
    suite_entry_t** at = &head;
    while (*at != NULL && strcmp((*at)->name, entry->name) <= 0) {
        at = &(*at)->next;
    }
    entry->next = *at;
    *at = entry;
    suite_count++;
}

size_t
suites_count(void) {
    return suite_count;
}

const suite_entry_t*
suites_at(size_t i) {
    /* Callers walk 0..count, so resume from the last entry reached. */
    static const suite_entry_t* cursor;
    static size_t cursor_index;
    if (i >= suite_count) {
        return NULL;
    }
    if (cursor == NULL || i < cursor_index) {
        cursor = head;
        cursor_index = 0;
    }
    for (; cursor_index < i; cursor_index++) {
        cursor = cursor->next;
    }
    return cursor;
}

void
suites_record_drop(void) {
    dropped++;
}

int
suites_dropped(void) {
    return dropped;
}

int
suites_run(bool on_request) {
    for (size_t i = 0; i < suites_count(); i++) {
        const suite_entry_t* suite = suites_at(i);
        if (suite->on_request == on_request) {
            suite->fn();
        }
    }
    return suites_dropped() + (suites_count() == 0);
}
