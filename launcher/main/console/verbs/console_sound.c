/* Development playback and inspection use the same queue as applications. */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "asset/asset_store.h"
#include "console/console_verbs.h"
#include "console/verbs/console_sound_parse.h"
#include "core/memory.h"
#include "sound/sound_engine.h"

#define CONSOLE_SOUND_REPLY_MAX 128

typedef struct console_sound_view {
    sound_t sound;
    char name[ASSET_NAME_MAX];
    struct console_sound_view* next;
} console_sound_view_t;

static console_sound_view_t* views;

static const sound_t*
console_sound_view(const char* name) {
    for (const console_sound_view_t* view = views; view; view = view->next) {
        if (strcmp(view->name, name) == 0) {
            return &view->sound;
        }
    }
    sound_t sound;
    if (sound_open(asset_store_pack(ASSET_ENGINE_PACK), name, &sound) != ASSET_OK) {
        return NULL;
    }
    console_sound_view_t* view = memory_alloc(sizeof *view, MEMORY_PSRAM);
    if (!view) {
        return NULL;
    }
    /* Commands return before playback ends; these immutable views live with the mounted engine pack. */
    *view = (console_sound_view_t){.sound = sound, .next = views};
    console_copy_text(view->name, sizeof view->name, name);
    views = view;
    return &view->sound;
}

static const char format_error[] = "SOUND_ERR formatting failed";

static void
sound_stats(console_reply_fn reply) {
    sound_stats_t stats;
    sound_get_stats(&stats);
    char line[CONSOLE_SOUND_REPLY_MAX];
    int length = snprintf(line, sizeof line, "SOUND ready=%u dropped=%" PRIu32 " amp=%u", stats.control.ready,
                          stats.control.dropped_commands, (unsigned)stats.amp);
    reply(length < 0 ? format_error : line);
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        const sound_control_voice_t* voice = &stats.control.voices[i];
        length = snprintf(line, sizeof line,
                          "SOUND slot=%d voice=%" PRIu32 " playing=%u bus=%u gain=%u priority=%u frame=%" PRIu32, i,
                          voice->handle.id, voice->playing, voice->opts.bus.id, voice->opts.gain_pct,
                          voice->opts.priority, stats.positions[i]);
        reply(length < 0 ? format_error : line);
    }
    length = snprintf(line, sizeof line, "SOUND ram voices=%zu queue=%zu stack=%zu tx_samples=%zu mixer=%zu",
                      stats.voice_table_bytes, stats.queue_bytes, stats.stack_bytes, stats.dma_sample_bytes,
                      stats.mixer_bytes);
    reply(length < 0 ? format_error : line);
    length = snprintf(line, sizeof line, "SOUND stack_min_free=%zu margin=%zu", stats.stack_min_free_bytes,
                      stats.stack_margin_bytes);
    reply(length < 0 ? format_error : line);
    reply("SOUND_END");
}

static void
console_verb_sound(const char* args, console_reply_fn reply) {
    console_sound_args_t parsed;
    if (!console_sound_parse(args, &parsed)) {
        reply("SOUND_ERR usage: sound play <name> [loop] [gain] | stop [voice|all] | stats");
        return;
    }
    if (parsed.action == CONSOLE_SOUND_STATS) {
        sound_stats(reply);
        return;
    }
    if (parsed.action == CONSOLE_SOUND_STOP) {
        if (parsed.voice) {
            sound_stop((sound_voice_t){.id = parsed.voice});
        } else {
            sound_stats_t stats;
            sound_get_stats(&stats);
            for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
                sound_stop(stats.control.voices[i].handle);
            }
        }
        reply("SOUND_END stopped");
        return;
    }
    const sound_t* sound = console_sound_view(parsed.name);
    if (!sound) {
        reply("SOUND_ERR sound unavailable");
        return;
    }
    const sound_play_opts_t opts = {
        .bus = SOUND_BUS_SFX, .gain_pct = parsed.gain_pct, .priority = SOUND_PRIORITY_NORMAL, .loop = parsed.loop};
    const sound_voice_t voice = sound_play(sound, &opts);
    if (!voice.id) {
        reply("SOUND_ERR playback unavailable");
        return;
    }
    char line[CONSOLE_SOUND_REPLY_MAX];
    const int length = snprintf(line, sizeof line, "SOUND_END voice=%" PRIu32, voice.id);
    reply(length < 0 ? format_error : line);
}
CONSOLE_VERB(sound, CONSOLE_LINE_MAX - sizeof("sound") - 1, console_verb_sound)
