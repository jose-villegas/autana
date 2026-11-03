/* console_perf_parse: PERF's words and replies, portable. The device's
 * console_perf.c supplies the frame_cost behind each callback. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "console/console_verbs.h"

typedef struct {
    int (*name_index)(const char* name);
    int (*event_index)(const char* event);
    const char* (*name_at)(int index);
    const char* (*event_at)(int index);
    int (*names_dropped)(void);
    void (*post_arm)(int name_index, int event_index);
} console_perf_ops_t;

#define CONSOLE_PERF_WORD_MAX 40

/* Copies the next space-separated word of `*at` into `word`; false at the
 * end of the line, and a word too long to keep is cut, never split in two. */
static inline bool
console_perf_next_word(const char** at, char* word, size_t word_size) {
    const char* p = *at;
    while (*p == ' ') {
        p++;
    }
    if (*p == '\0') {
        return false;
    }
    size_t length = 0;
    while (*p != ' ' && *p != '\0') {
        if (length + 1 < word_size) {
            word[length++] = *p;
        }
        p++;
    }
    word[length] = '\0';
    *at = p;
    return true;
}

static inline void
console_perf_list(const console_perf_ops_t* ops, console_reply_fn reply) {
    char line[CONSOLE_PERF_WORD_MAX + 16];
    for (int i = 0; ops->name_at(i) != NULL; i++) {
        if (snprintf(line, sizeof line, "PERFMON_NAME %s", ops->name_at(i)) > 0) {
            reply(line);
        }
    }
    if (ops->names_dropped() > 0 && snprintf(line, sizeof line, "PERFMON_NAMES_DROPPED %d", ops->names_dropped()) > 0) {
        reply(line);
    }
    for (int i = 0; ops->event_at(i) != NULL; i++) {
        if (snprintf(line, sizeof line, "PERFMON_EVENT %s", ops->event_at(i)) > 0) {
            reply(line);
        }
    }
    reply("PERFMON_END");
}

/* PERF [?] lists; PERF off disarms; PERF <name> [event] arms. The event
 * defaults to the first one the device lists. */
static inline void
console_perf_dispatch(const char* args, const console_perf_ops_t* ops, console_reply_fn reply) {
    char name[CONSOLE_PERF_WORD_MAX];
    char event[CONSOLE_PERF_WORD_MAX] = "";
    char extra[CONSOLE_PERF_WORD_MAX];
    const char* at = args;
    if (!console_perf_next_word(&at, name, sizeof name) || strcmp(name, "?") == 0) {
        console_perf_list(ops, reply);
        return;
    }
    const bool has_event = console_perf_next_word(&at, event, sizeof event);
    if (console_perf_next_word(&at, extra, sizeof extra)) {
        reply("PERFMON_ERR usage: PERF <name|off|?> [event]");
        return;
    }
    char line[2 * CONSOLE_PERF_WORD_MAX + 24];
    if (strcmp(name, "off") == 0 && !has_event) {
        ops->post_arm(-1, 0);
        reply("PERFMON_OK off");
        return;
    }
    const int name_index = ops->name_index(name);
    if (name_index < 0) {
        if (snprintf(line, sizeof line, "PERFMON_ERR unknown name %s", name) > 0) {
            reply(line);
        }
        return;
    }
    const int event_index = has_event ? ops->event_index(event) : 0;
    if (event_index < 0) {
        if (snprintf(line, sizeof line, "PERFMON_ERR unknown event %s", event) > 0) {
            reply(line);
        }
        return;
    }
    ops->post_arm(name_index, event_index);
    if (snprintf(line, sizeof line, "PERFMON_OK %s %s", name, ops->event_at(event_index)) > 0) {
        reply(line);
    }
}
