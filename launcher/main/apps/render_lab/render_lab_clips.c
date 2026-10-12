#include "apps/render_lab/render_lab_clips.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/memory.h"
#include "esp_log.h"
#include "gfx/draw/gfx_draw.h"
#include "ui/ui.h"
#include "ui/ui_widgets.h"

static int render_lab_clips_find(const scene_t* scene, scene_entity_t subject, const char* name);
static bool render_lab_clips_play(render_lab_clips_t* choice, int clip, int blend_ms);

bool
render_lab_clips_init(render_lab_clips_t* choice, scene_t* scene, scene_entity_t subject, const char* default_clip) {
    *choice = (render_lab_clips_t){.scene = scene, .subject = subject, .blend_ms = RENDER_LAB_BLEND_DEFAULT_MS};
    const int count = scene == NULL ? 0 : scene_animator_clip_count(scene, subject);
    if (count == 0) {
        return true;
    }
    choice->selected = render_lab_clips_find(scene, subject, default_clip);
    if (choice->selected < 0) {
        choice->selected = 0;
        ESP_LOGW("render_clips", "default clip missing");
    }
    (void)render_lab_clips_play(choice, choice->selected, 0);
    choice->blend_ms = RENDER_LAB_BLEND_DEFAULT_MS;
    ui_dropdown_item_t* items = memory_alloc((size_t)count * sizeof *items, MEMORY_PSRAM);
    if (items == NULL) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        items[i] = (ui_dropdown_item_t){.label = scene_animator_clip_name(scene, subject, i)};
    }
    choice->items = items;
    choice->count = count;
    return true;
}

void
render_lab_clips_destroy(render_lab_clips_t* choice) {
    memory_free(choice->items);
    *choice = (render_lab_clips_t){0};
}

static int
render_lab_clips_find(const scene_t* scene, scene_entity_t subject, const char* name) {
    if (scene == NULL) {
        return -1;
    }
    for (int i = 0; i < scene_animator_clip_count(scene, subject); i++) {
        if (strcmp(scene_animator_clip_name(scene, subject, i), name) == 0) {
            return i;
        }
    }
    return -1;
}

static bool
render_lab_clips_play(render_lab_clips_t* choice, int clip, int blend_ms) {
    if (choice->scene == NULL || blend_ms < 0 || blend_ms > RENDER_LAB_BLEND_MAX_MS
        || !scene_animator_play(choice->scene, choice->subject, clip, (uint32_t)blend_ms)) {
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
    if (end == text || *end != '\0' || value < 0 || value > RENDER_LAB_BLEND_MAX_MS) {
        return false;
    }
    *blend_ms = (int)value;
    return true;
}

static void
play_request(render_lab_clips_t* choice, const char* args, console_reply_fn reply) {
    const char* space = strchr(args, ' ');
    const size_t length = space == NULL ? strlen(args) : (size_t)(space - args);
    char name[ASSET_NAME_MAX];
    if (length == 0 || length >= sizeof name) {
        reply("RENDER_ERR bad clip name");
        return;
    }
    memcpy(name, args, length);
    name[length] = '\0';
    int blend_ms = choice->blend_ms;
    if (space != NULL && !parse_blend(space + 1, &blend_ms)) {
        reply("RENDER_ERR bad blend");
        return;
    }
    const int clip = render_lab_clips_find(choice->scene, choice->subject, name);
    char line[ASSET_NAME_MAX + sizeof("RENDER clip= blend_ms=1000")];
    if (!render_lab_clips_play(choice, clip, blend_ms)) {
        (void)snprintf(line, sizeof line, "RENDER_ERR unknown clip '%s'", name);
        reply(line);
        return;
    }
    (void)snprintf(line, sizeof line, "RENDER clip=%s blend_ms=%d", name, blend_ms);
    reply(line);
    reply("RENDER_END");
}

bool
render_lab_clips_reply(render_lab_clips_t* choice, const char* args, console_reply_fn reply) {
    const char* rest;
    const bool listing = console_word_match(args, "clips", &rest) && *rest == '\0';
    if (!listing && !console_word_match(args, "clip", &rest)) {
        return false;
    }
    const scene_t* scene = choice->scene;
    const scene_entity_t subject = choice->subject;
    if (scene == NULL || scene_animator_clip_count(scene, subject) == 0) {
        reply("RENDER_ERR no animated subject");
    } else if (listing) {
        for (int i = 0; i < scene_animator_clip_count(scene, subject); i++) {
            char line[ASSET_NAME_MAX + sizeof("RENDER clip= current=1")];
            (void)snprintf(line, sizeof line, "RENDER clip=%s current=%d", scene_animator_clip_name(scene, subject, i),
                           i == choice->selected);
            reply(line);
        }
        reply("RENDER_END");
    } else {
        play_request(choice, rest, reply);
    }
    return true;
}

bool
render_lab_clips_console(render_lab_clips_t* choice, const char* args) {
    const bool handled = render_lab_clips_reply(choice, args, console_reply_stdio);
    return handled;
}

void
render_lab_clips_draw(mu_Context* ctx, ui_flow_t* flow, int width, render_lab_clips_t* choice) {
    if (choice->count == 0) {
        return;
    }
    const ui_theme_t theme = ui_theme_from_style(ctx->style);
    ui_flow_row(ctx, flow, width, gfx_text_height());
    mu_label(ctx, "CLIP");
    ui_flow_row(ctx, flow, width, UI_ROW_HEIGHT);
    const mu_Rect anchor = mu_layout_next(ctx);
    const int picked = ui_dropdown(ctx, "clip", anchor, choice->items, choice->count, choice->selected, &theme);
    if (picked >= 0) {
        (void)render_lab_clips_play(choice, picked, choice->blend_ms);
    }
    char label[sizeof("BLEND: 1000 ms")];
    (void)snprintf(label, sizeof label, "BLEND: %d ms", choice->blend_ms);
    ui_flow_row(ctx, flow, width, gfx_text_height());
    mu_label(ctx, label);
    ui_flow_row(ctx, flow, width, UI_ROW_HEIGHT);
    (void)ui_slider_int(ctx, &choice->blend_ms, 0, RENDER_LAB_BLEND_MAX_MS, RENDER_LAB_BLEND_STEP_MS);
}
