/* Frame-owned sound views keep their mapped packs alive. */
#include "console/verbs/console_sound_views.h"

#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "console/console_verbs.h"
#include "console/verbs/console_tone.h"
#include "core/memory.h"
#include "core/timing.h"
#include "esp_log.h"
#include "sound/sound_engine.h"

typedef struct console_sound_view {
    handle_t load;
    sound_t sound;
    /* At most SOUND_VOICE_COUNT handles per view can remain live. */
    sound_voice_t voices[SOUND_VOICE_COUNT];
    char name[ASSET_NAME_MAX];
    char pack[ASSET_NAME_MAX];
    struct console_sound_view* next;
} console_sound_view_t;

static const char* TAG = "console_sound";
static console_sound_view_t* views;
static bool release_warned;

const sound_t*
console_sound_view(const console_sound_args_t* args, int64_t* open_us) {
    handle_t shared = HANDLE_NONE; /* one load per pack: the views of a pack are released together */
    for (const console_sound_view_t* view = views; view; view = view->next) {
        if (strcmp(view->pack, args->pack) == 0) {
            if (strcmp(view->name, args->name) == 0) {
                return &view->sound;
            }
            shared = handle_is_none(view->load) ? shared : view->load;
        }
    }
    const handle_t load = handle_is_none(shared) ? asset_store_load(args->pack) : HANDLE_NONE;
    const asset_pack_t* pack = asset_store_loaded(handle_is_none(shared) ? load : shared);
    if (!pack) {
        asset_store_release(load);
        return NULL;
    }
    sound_t sound;
    const int64_t began = timing_now_us();
    const asset_status_t status = sound_open(pack, args->name, &sound);
    *open_us = timing_now_us() - began;
    if (status != ASSET_OK) {
        asset_store_release(load);
        return NULL;
    }
    console_sound_view_t* view = memory_alloc(sizeof *view, MEMORY_PSRAM);
    if (!view) {
        asset_store_release(load);
        return NULL;
    }
    *view = (console_sound_view_t){.load = load, .sound = sound, .next = views};
    console_copy_text(view->name, sizeof view->name, args->name);
    console_copy_text(view->pack, sizeof view->pack, args->pack);
    views = view;
    return &view->sound;
}

static bool
fence_views(void) {
    for (const console_sound_view_t* view = views; view; view = view->next) {
        for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
            sound_voice_t voice = view->voices[i];
            if (voice.id && sound_is_playing(voice) && !console_tone_stop(&voice)) {
                return false;
            }
        }
    }
    return sound_fence();
}

void
console_sound_release_views(void) {
    if (!views) {
        return;
    }
    if (!fence_views()) {
        if (!release_warned) {
            ESP_LOGW(TAG, "sound release timed out; views kept");
            release_warned = true;
        }
        return;
    }
    release_warned = false;
    console_sound_view_t** link = &views;
    while (*link) {
        console_sound_view_t* view = *link;
        *link = view->next;
        asset_store_release(view->load);
        memory_free(view);
    }
}

void
console_sound_track_voice(const sound_t* sound, sound_voice_t voice) {
    for (console_sound_view_t* view = views; view; view = view->next) {
        if (&view->sound == sound) {
            for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
                if (!sound_is_playing(view->voices[i])) {
                    view->voices[i] = voice;
                    return;
                }
            }
            abort();
        }
    }
}
