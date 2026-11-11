#include "lab/lab_clips.h"
#include "lab/lab.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app/app_arena.h"
#include "esp_log.h"
#include "lab/ui/lab_menu_screen.h"
#include "scene/animator/animator.h"
#include "ui/ui.h"

static int lab_clips_find(entity_t scene, entity_t subject, const char* name);
static bool lab_clips_play(lab_clips_t* choice, int clip, int blend_ms);

bool
lab_clips_init(lab_clips_t* choice, const char* tag, entity_t scene, entity_t subject, const char* default_clip) {
    *choice = (lab_clips_t){.tag = tag, .scene = scene, .subject = subject, .blend_ms = LAB_CLIPS_BLEND_DEFAULT_MS};
    const int count = handle_is_none(scene) ? 0 : animator_clip_count(subject);
    if (count == 0) {
        return true;
    }
    choice->selected = lab_clips_find(scene, subject, default_clip);
    if (choice->selected < 0) {
        choice->selected = 0;
        ESP_LOGW("render_clips", "default clip missing");
    }
    (void)animator_play(subject, choice->selected, 0);
    const char** items = app_arena_take((size_t)count * sizeof *items, _Alignof(const char*));
    if (items == NULL) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        items[i] = animator_clip_name(subject, i);
    }
    choice->items = items;
    choice->count = count;
    return true;
}

void
lab_clips_destroy(lab_clips_t* choice) {
    *choice = (lab_clips_t){0};
}

static int
lab_clips_find(entity_t scene, entity_t subject, const char* name) {
    if (handle_is_none(scene)) {
        return -1;
    }
    for (int i = 0; i < animator_clip_count(subject); i++) {
        if (strcmp(animator_clip_name(subject, i), name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool
lab_clips_play(lab_clips_t* choice, int clip, int blend_ms) {
    if (handle_is_none(choice->scene) || blend_ms < 0 || blend_ms > LAB_CLIPS_BLEND_MAX_MS
        || !animator_play(choice->subject, clip, (uint32_t)blend_ms)) {
        return false;
    }
    choice->selected = clip;
    choice->blend_ms = blend_ms;
    return true;
}

static bool
parse_blend(const char* text, int* blend_ms) {
    char* end;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < INT_MIN || value > INT_MAX) {
        return false;
    }
    *blend_ms = (int)value;
    return true;
}

/* One formatted reply line, long enough for a clip name and its longest field. */
static void
reply_line(console_reply_fn reply, const char* format, ...) {
    char line[LAB_WORD_MAX + ASSET_NAME_MAX + sizeof("_ERR unknown clip ''") + LAB_CLIPS_BLEND_TEXT_SIZE];
    va_list args;
    va_start(args, format);
    if (vsnprintf(line, sizeof line, format, args) < 0) {
        line[0] = '\0';
    }
    va_end(args);
    reply(line);
}

static void
play_request(lab_clips_t* choice, const char* args, console_reply_fn reply) {
    const char* space = strchr(args, ' ');
    const size_t length = space == NULL ? strlen(args) : (size_t)(space - args);
    char name[ASSET_NAME_MAX];
    if (length == 0 || length >= sizeof name) {
        reply_line(reply, "%s_ERR bad clip name", choice->tag);
        return;
    }
    memcpy(name, args, length);
    name[length] = '\0';
    int blend_ms = choice->blend_ms;
    if (space != NULL && !parse_blend(space + 1, &blend_ms)) {
        reply_line(reply, "%s_ERR bad blend", choice->tag);
        return;
    }
    const int clip = lab_clips_find(choice->scene, choice->subject, name);
    if (!lab_clips_play(choice, clip, blend_ms)) {
        if (clip < 0) {
            reply_line(reply, "%s_ERR unknown clip '%s'", choice->tag, name);
        } else {
            reply_line(reply, "%s_ERR bad blend", choice->tag);
        }
        return;
    }
    reply_line(reply, "%s clip=%s blend_ms=%d", choice->tag, name, blend_ms);
    reply_line(reply, "%s_END", choice->tag);
}

bool
lab_clips_reply(lab_clips_t* choice, const char* args, console_reply_fn reply) {
    const char* rest;
    const bool listing = console_word_match(args, "clips", &rest) && *rest == '\0';
    if (!listing && !console_word_match(args, "clip", &rest)) {
        return false;
    }
    entity_t scene = choice->scene;
    const entity_t subject = choice->subject;
    if (handle_is_none(scene) || animator_clip_count(subject) == 0) {
        reply_line(reply, "%s_ERR no animated subject", choice->tag);
    } else if (listing) {
        for (int i = 0; i < animator_clip_count(subject); i++) {
            reply_line(reply, "%s clip=%s current=%d", choice->tag, animator_clip_name(subject, i),
                       i == choice->selected);
        }
        reply_line(reply, "%s_END", choice->tag);
    } else {
        play_request(choice, rest, reply);
    }
    return true;
}

void
lab_clips_draw(int width, lab_clips_t* choice) {
    if (choice->count == 0) {
        return;
    }
    lab_menu_caption(width, "CLIP");
    ui_row_centre(width, UI_ROW_HEIGHT);
    int picked = choice->selected;
    if (ui_dropdown(ui_next(), "CLIP", &picked, choice->items, choice->count)) {
        (void)lab_clips_play(choice, picked, choice->blend_ms);
    }
    char label[sizeof("BLEND:  ms") + LAB_CLIPS_BLEND_TEXT_SIZE];
    if (snprintf(label, sizeof label, "BLEND: %d ms", choice->blend_ms) < 0) {
        label[0] = '\0';
    }
    lab_menu_caption(width, label);
    /* The slider moves in whole steps, so it counts steps, not milliseconds. */
    ui_row_centre(width, UI_ROW_HEIGHT);
    int steps = choice->blend_ms / LAB_CLIPS_BLEND_STEP_MS;
    if (ui_slider(ui_next(), "BLEND", &steps, 0, LAB_CLIPS_BLEND_MAX_MS / LAB_CLIPS_BLEND_STEP_MS)) {
        choice->blend_ms = steps * LAB_CLIPS_BLEND_STEP_MS;
    }
}
