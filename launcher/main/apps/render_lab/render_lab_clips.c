#include "apps/render_lab/render_lab_clips.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/memory.h"
#include "gfx/draw/gfx_draw.h"
#include "ui/ui.h"
#include "ui/ui_widgets.h"

bool
render_lab_clips_init(const scene_t* scene, scene_entity_t subject, render_lab_clips_t* choice) {
    const int count = scene == NULL ? 0 : scene_animator_clip_count(scene, subject);
    if (count == 0) {
        return true;
    }
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
    choice->items = NULL;
    choice->count = 0;
}

int
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

bool
render_lab_clips_play(scene_t* scene, scene_entity_t subject, render_lab_clips_t* choice, int clip, int blend_ms) {
    if (scene == NULL || blend_ms < 0 || blend_ms > RENDER_LAB_BLEND_MAX_MS
        || !scene_animator_play(scene, subject, clip, (uint32_t)blend_ms)) {
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
play_request(scene_t* scene, scene_entity_t subject, render_lab_clips_t* choice, const char* args) {
    const char* space = strchr(args, ' ');
    const size_t length = space == NULL ? strlen(args) : (size_t)(space - args);
    char name[ASSET_NAME_MAX];
    if (length == 0 || length >= sizeof name) {
        (void)printf("RENDER_ERR bad clip name\n");
        return;
    }
    memcpy(name, args, length);
    name[length] = '\0';
    int blend_ms = choice->blend_ms;
    if (space != NULL && !parse_blend(space + 1, &blend_ms)) {
        (void)printf("RENDER_ERR bad blend\n");
        return;
    }
    const int clip = render_lab_clips_find(scene, subject, name);
    if (!render_lab_clips_play(scene, subject, choice, clip, blend_ms)) {
        (void)printf("RENDER_ERR unknown clip '%s'\n", name);
        return;
    }
    (void)printf("RENDER clip=%s blend_ms=%d\nRENDER_END\n", name, blend_ms);
}

bool
render_lab_clips_console(scene_t* scene, scene_entity_t subject, render_lab_clips_t* choice, const char* args) {
    const bool listing = strcmp(args, "clips") == 0;
    if (!listing && strncmp(args, "clip ", sizeof("clip ") - 1) != 0 && strcmp(args, "clip") != 0) {
        return false;
    }
    if (scene == NULL || scene_animator_clip_count(scene, subject) == 0) {
        (void)printf("RENDER_ERR no animated subject\n");
    } else if (listing) {
        for (int i = 0; i < scene_animator_clip_count(scene, subject); i++) {
            (void)printf("RENDER clip=%s current=%d\n", scene_animator_clip_name(scene, subject, i),
                         i == choice->selected);
        }
        (void)printf("RENDER_END\n");
    } else {
        play_request(scene, subject, choice, args + (strcmp(args, "clip") == 0 ? strlen(args) : sizeof("clip ") - 1));
    }
    (void)fflush(stdout);
    return true;
}

void
render_lab_clips_draw(mu_Context* ctx, ui_flow_t* flow, int width, scene_t* scene, scene_entity_t subject,
                      render_lab_clips_t* choice) {
    if (choice->count == 0) {
        return;
    }
    const ui_theme_t theme = {.button_face = ctx->style->colors[MU_COLOR_BUTTON],
                              .accent_face = ctx->style->colors[MU_COLOR_BUTTONFOCUS],
                              .text = ctx->style->colors[MU_COLOR_TEXT],
                              .on_accent = ctx->style->colors[MU_COLOR_TEXT],
                              .text_scale = GFX_GLYPH_SCALE};
    ui_flow_row(ctx, flow, width, gfx_text_height());
    mu_label(ctx, "CLIP");
    ui_flow_row(ctx, flow, width, UI_ROW_HEIGHT);
    const mu_Rect anchor = mu_layout_next(ctx);
    const int picked = ui_dropdown(ctx, "clip", anchor, choice->items, choice->count, choice->selected, &theme);
    if (picked >= 0) {
        (void)render_lab_clips_play(scene, subject, choice, picked, choice->blend_ms);
    }
    char label[sizeof("BLEND: 1000 ms")];
    (void)snprintf(label, sizeof label, "BLEND: %d ms", choice->blend_ms);
    ui_flow_row(ctx, flow, width, gfx_text_height());
    mu_label(ctx, label);
    ui_flow_row(ctx, flow, width, UI_ROW_HEIGHT);
    (void)ui_slider_int(ctx, &choice->blend_ms, 0, RENDER_LAB_BLEND_MAX_MS, RENDER_LAB_BLEND_STEP_MS);
}
