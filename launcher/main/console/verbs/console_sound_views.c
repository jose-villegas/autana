/* Frame-owned sound views keep their mapped packs alive. */
#include "console/verbs/console_sound.h"

#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "core/timing.h"
#include "sound/sound_engine.h"

typedef struct console_sound_view {
    sound_t sound;
    /* Later successful plays steal slots before this history can lose a live handle. */
    sound_voice_t voices[SOUND_VOICE_COUNT];
    unsigned next_voice;
    char name[ASSET_NAME_MAX];
    char pack[ASSET_NAME_MAX];
    struct console_sound_view* next;
} console_sound_view_t;

static console_sound_view_t* views;

const sound_t*
console_sound_view(const console_sound_args_t* args, int64_t* open_us) {
    for (const console_sound_view_t* view = views; view; view = view->next) {
        if (strcmp(view->name, args->name) == 0 && strcmp(view->pack, args->pack) == 0) {
            return &view->sound;
        }
    }
    const asset_pack_t* pack = asset_store_pack(args->pack);
    if (!pack) {
        return NULL;
    }
    sound_t sound;
    const int64_t began = timing_now_us();
    const asset_status_t status = sound_open(pack, args->name, &sound);
    *open_us = timing_now_us() - began;
    if (status != ASSET_OK) {
        asset_store_release(args->pack);
        return NULL;
    }
    console_sound_view_t* view = memory_alloc(sizeof *view, MEMORY_PSRAM);
    if (!view) {
        asset_store_release(args->pack);
        return NULL;
    }
    *view = (console_sound_view_t){.sound = sound, .next = views};
    console_copy_text(view->name, sizeof view->name, args->name);
    console_copy_text(view->pack, sizeof view->pack, args->pack);
    views = view;
    return &view->sound;
}

void
console_sound_app_exit(void) {
    console_sound_view_t** link = &views;
    while (*link) {
        console_sound_view_t* view = *link;
        *link = view->next;
        asset_store_release(view->pack);
        memory_free(view);
    }
}

void
console_sound_track_voice(const sound_t* sound, sound_voice_t voice) {
    for (console_sound_view_t* view = views; view; view = view->next) {
        if (&view->sound == sound) {
            view->voices[view->next_voice] = voice;
            view->next_voice = (view->next_voice + 1) % SOUND_VOICE_COUNT;
            return;
        }
    }
}

void
console_sound_app_enter(void) {
    if (views && !sound_app_active()) {
        const int64_t deadline = timing_now_us() + ((int64_t)SOUND_EXIT_READ_TIMEOUT_MS * MATH_US_PER_MS);
        for (const console_sound_view_t* view = views; view; view = view->next) {
            for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
                const sound_voice_t voice = view->voices[i];
                while (sound_is_playing(voice)) {
                    if (timing_now_us() >= deadline) {
                        abort();
                    }
                    sound_stop(voice);
                    if (sound_is_playing(voice)) {
                        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
                    }
                }
            }
        }
        sound_fence();
        console_sound_app_exit();
    }
}
