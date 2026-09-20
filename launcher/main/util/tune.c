#include "util/tune.h"

/* A release build has no tunables: the file is compiled for it, since the
 * source list is not conditional, and comes to nothing. */
#if TUNE_ENABLED

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REPLY_MAX 96

static tune_entry_t entries[TUNE_MAX];
static int entry_count;
static uint32_t generation;

static tune_entry_t*
find(const char* name) {
    for (int i = 0; i < entry_count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }
    return NULL;
}

bool
tune_register(const char* name, int32_t* value, int32_t low, int32_t high) {
    if (strlen(name) > TUNE_NAME_MAX) {
        return false;
    }
    tune_entry_t* entry = find(name);
    if (entry == NULL) {
        if (entry_count == TUNE_MAX) {
            return false;
        }
        entry = &entries[entry_count++];
    }
    *entry = (tune_entry_t){name, value, low, high};
    return true;
}

int
tune_count(void) {
    return entry_count;
}

const tune_entry_t*
tune_at(int index) {
    return index >= 0 && index < entry_count ? &entries[index] : NULL;
}

uint32_t
tune_generation(void) {
    return generation;
}

void
tune_reset(void) {
    entry_count = 0;
    generation = 0;
}

static void
reply_value(tune_reply_fn reply, const tune_entry_t* entry) {
    char line[REPLY_MAX];
    snprintf(line, sizeof line, "TUNE_OK %s=%ld", entry->name, (long)*entry->value);
    reply(line);
}

static void
reply_error(tune_reply_fn reply, const char* reason, const char* about) {
    char line[REPLY_MAX];
    snprintf(line, sizeof line, "TUNE_ERR %s %s", reason, about);
    reply(line);
}

static void
list_all(tune_reply_fn reply) {
    char line[REPLY_MAX];
    for (int i = 0; i < entry_count; i++) {
        snprintf(line, sizeof line, "TUNE %s=%ld min=%ld max=%ld", entries[i].name, (long)*entries[i].value,
                 (long)entries[i].low, (long)entries[i].high);
        reply(line);
    }
    snprintf(line, sizeof line, "TUNE_END count=%d", entry_count);
    reply(line);
}

/* `rest` is what follows "SET ": a name, one space, a whole number. */
static void
set_from(const char* rest, tune_reply_fn reply) {
    char name[TUNE_NAME_MAX + 1];
    const char* space = strchr(rest, ' ');
    if (space == NULL || space == rest || (size_t)(space - rest) > TUNE_NAME_MAX) {
        reply_error(reply, "usage", "SET <name> <value>");
        return;
    }
    memcpy(name, rest, (size_t)(space - rest));
    name[space - rest] = '\0';

    tune_entry_t* entry = find(name);
    if (entry == NULL) {
        reply_error(reply, "unknown", name);
        return;
    }
    char* end = NULL;
    const long value = strtol(space + 1, &end, 0);
    if (end == space + 1 || *end != '\0') {
        reply_error(reply, "not-a-number", space + 1);
        return;
    }
    if (value < entry->low || value > entry->high) {
        char range[TUNE_NAME_MAX + sizeof " takes -2147483648..-2147483648"];
        snprintf(range, sizeof range, "%s takes %ld..%ld", name, (long)entry->low, (long)entry->high);
        reply_error(reply, "range", range);
        return;
    }
    *entry->value = (int32_t)value;
    generation++;
    reply_value(reply, entry);
}

bool
tune_handle_line(const char* line, tune_reply_fn reply) {
    if (strcmp(line, "TUNE") == 0) {
        list_all(reply);
        return true;
    }
    if (strncmp(line, "GET ", 4) == 0) {
        const tune_entry_t* entry = find(line + 4);
        if (entry == NULL) {
            reply_error(reply, "unknown", line + 4);
        } else {
            reply_value(reply, entry);
        }
        return true;
    }
    if (strncmp(line, "SET ", 4) == 0) {
        set_from(line + 4, reply);
        return true;
    }
    return false;
}

#else

typedef int tune_has_nothing_in_a_release_build_t;

#endif
