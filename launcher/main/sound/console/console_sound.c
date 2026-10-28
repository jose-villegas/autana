/* Development playback and inspection use the same queue as applications. */
#include "sound/console/console_sound.h"
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "audio/audio_in.h"
#include "audio/audio_loopback.h"
#include "audio/audio_out_device.h"
#include "console/console_frame_request.h"
#include "console/console_verbs.h"
#include "core/memory.h"
#include "core/timing.h"
#include "sound/console/console_sound_parse.h"
#include "sound/console/console_sound_views.h"
#include "sound/sound_engine.h"

#define CONSOLE_SOUND_REPLY_MAX 128

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

#define ONSET_BLOCK_MS                                                                                                 \
    ((SOUND_MIXER_BLOCK_FRAMES * MATH_MS_PER_S + AUDIO_OUT_SAMPLE_RATE_HZ - 1) / AUDIO_OUT_SAMPLE_RATE_HZ)
#define ONSET_MARGIN_MS              (AUDIO_OUT_AMP_STARTUP_MS + (AUDIO_CODEC_DMA_BLOCKS * ONSET_BLOCK_MS))
#define ONSET_PRE_MS                 AUDIO_OUT_AMP_STARTUP_MS
#define ONSET_SHORT_MS               ONSET_MARGIN_MS
#define ONSET_DB_FLOOR               (-120.0f)
#define ONSET_DB_SCALE               20.0f
#define ONSET_DIFFERENCE_COEFFICIENT 2

typedef struct {
    int32_t previous;
    int32_t previous_previous;
    unsigned primed;
    int32_t peak;
    size_t peak_frame;
} onset_peak_t;

static bool
onset_window(onset_peak_t* peak, uint32_t ms) {
    int16_t samples[SOUND_MIXER_BLOCK_FRAMES];
    peak->peak = 0;
    peak->peak_frame = 0;
    const size_t frames = audio_out_samples(ms);
    for (size_t offset = 0; offset < frames;) {
        const size_t count = frames - offset < SOUND_MIXER_BLOCK_FRAMES ? frames - offset : SOUND_MIXER_BLOCK_FRAMES;
        if (!audio_in_read(samples, count)) {
            return false;
        }
        for (size_t i = 0; i < count; i++) {
            const int32_t sample = samples[i];
            const int32_t difference =
                (sample - (ONSET_DIFFERENCE_COEFFICIENT * peak->previous)) + peak->previous_previous;
            peak->previous_previous = peak->previous;
            peak->previous = sample;
            if (peak->primed < ONSET_DIFFERENCE_COEFFICIENT) {
                peak->primed++;
            } else if (abs(difference) > peak->peak) {
                peak->peak = abs(difference);
                peak->peak_frame = offset + i;
            }
        }
        offset += count;
    }
    return true;
}

static float
onset_db(int32_t peak) {
    return peak ? ONSET_DB_SCALE * log10f((float)peak / INT16_MAX) : ONSET_DB_FLOOR;
}

static void
stop_all(void) {
    sound_stats_t stats;
    sound_get_stats(&stats);
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        sound_stop(stats.control.voices[i].handle);
    }
}

static bool
onset_wait(onset_peak_t* peak) {
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    do {
        sound_stats_t stats;
        sound_get_stats(&stats);
        if (!stats.control.ready) {
            return false;
        }
        if (stats.amp == SOUND_AMP_ON) {
            return true;
        }
        if (!onset_window(peak, ONSET_BLOCK_MS)) {
            return false;
        }
    } while (timing_now_us() < deadline);
    return false;
}

static bool
onset_prepare(const console_sound_args_t* args, onset_peak_t* peak, sound_voice_t* probe) {
    stop_all();
    if (args->event == CONSOLE_SOUND_AMP_ON && !audio_out_amp_hold(true)) {
        return false;
    }
    if (args->event == CONSOLE_SOUND_STOP_EVENT || args->event == CONSOLE_SOUND_STEAL) {
        *probe = sound_play_tone(args->hz, SOUND_TONE_UNTIL_STOPPED, args->gain_pct, SOUND_BUS_SYSTEM);
        if (!probe->id) {
            return false;
        }
    }
    const unsigned holders = args->event == CONSOLE_SOUND_STEAL ? SOUND_VOICE_COUNT - 1 : 1;
    for (unsigned i = 0; i < holders; i++) {
        if (!sound_play_tone(args->hz, SOUND_TONE_UNTIL_STOPPED, 0, SOUND_BUS_SYSTEM).id
            || !onset_window(peak, ONSET_BLOCK_MS)) {
            return false;
        }
    }
    if (!onset_wait(peak) || !onset_window(peak, ONSET_MARGIN_MS)) {
        return false;
    }
    return true;
}

static bool
onset_trigger(const console_sound_args_t* args, sound_voice_t probe) {
    switch (args->event) {
        case CONSOLE_SOUND_START:
            if (!sound_play_tone(args->hz, SOUND_TONE_UNTIL_STOPPED, args->gain_pct, SOUND_BUS_SYSTEM).id) {
                return false;
            }
            break;
        case CONSOLE_SOUND_STOP_EVENT: sound_stop(probe); break;
        case CONSOLE_SOUND_AMP_ON: return audio_out_amp_hold(false);
        case CONSOLE_SOUND_STEAL:
            if (!sound_play_tone(args->hz, SOUND_TONE_UNTIL_STOPPED, 0, SOUND_BUS_SYSTEM).id) {
                return false;
            }
            break;
        default: break;
    }
    return true;
}

static bool
onset_measure(const console_sound_args_t* args, onset_peak_t* peak, float* pre_db) {
    sound_voice_t probe = SOUND_VOICE_NULL;
    if (!onset_prepare(args, peak, &probe)) {
        return false;
    }
    if (!onset_window(peak, ONSET_PRE_MS)) {
        return false;
    }
    *pre_db = onset_db(peak->peak);
    if (!onset_trigger(args, probe)) {
        return false;
    }
    const uint32_t window =
        args->event == CONSOLE_SOUND_AMP_ON ? AUDIO_OUT_AMP_STARTUP_MS + ONSET_MARGIN_MS : ONSET_SHORT_MS;
    return onset_window(peak, window);
}

static void
sound_onset(const console_sound_args_t* args, console_reply_fn reply) {
    if (!sound_capture_open()) {
        (void)sound_capture_close();
        reply("SOUND_ERR onset capture open");
        return;
    }
    onset_peak_t peak = {0};
    float pre_db = 0;
    const bool measured = onset_window(&peak, AUDIO_LOOPBACK_SETTLE_MS) && onset_measure(args, &peak, &pre_db);
    stop_all();
    const bool released = audio_out_amp_hold(false);
    const bool closed = sound_capture_close();
    if (!measured || !released || !closed) {
        reply("SOUND_ERR onset capture");
        return;
    }
    char line[CONSOLE_SOUND_REPLY_MAX];
    const float peak_db = onset_db(peak.peak);
    const int length = snprintf(
        line, sizeof line, "SOUND onset event=%s hz=%u gain=%u pre_db=%.1f peak_db=%.1f excess_db=%.1f at_ms=%u",
        console_sound_events[args->event], args->hz, args->gain_pct, (double)pre_db, (double)peak_db,
        (double)(peak_db - pre_db), (unsigned)(peak.peak_frame * MATH_MS_PER_S / AUDIO_OUT_SAMPLE_RATE_HZ));
    if (length < 0 || (size_t)length >= sizeof line) {
        reply(format_error);
        return;
    }
    reply(line);
    reply("SOUND_END");
}

static void
sound_run_parsed(const console_sound_args_t* parsed, console_reply_fn reply) {
    if (parsed->action == CONSOLE_SOUND_ONSET) {
        sound_onset(parsed, reply);
        return;
    }
    if (parsed->action == CONSOLE_SOUND_STATS) {
        sound_stats(reply);
        return;
    }
    if (parsed->action == CONSOLE_SOUND_STOP) {
        if (parsed->voice) {
            sound_stop((sound_voice_t){.id = parsed->voice});
        } else {
            stop_all();
        }
        reply("SOUND_END stopped");
        return;
    }
    int64_t open_us = 0;
    const sound_t* sound = console_sound_view(parsed, &open_us);
    if (!sound) {
        reply("SOUND_ERR sound unavailable");
        return;
    }
    const sound_play_opts_t opts = {
        .bus = SOUND_BUS_SFX, .gain_pct = parsed->gain_pct, .priority = SOUND_PRIORITY_NORMAL, .loop = parsed->loop};
    const sound_voice_t voice = sound_play(sound, &opts);
    if (!voice.id) {
        reply("SOUND_ERR playback unavailable");
        return;
    }
    console_sound_track_voice(sound, voice);
    char line[CONSOLE_SOUND_REPLY_MAX];
    const int length = snprintf(line, sizeof line, "SOUND_END voice=%" PRIu32 " open_us=%" PRId64, voice.id, open_us);
    reply(length < 0 ? format_error : line);
}

static void
answer(const void* payload) {
    sound_run_parsed(payload, console_reply_stdio);
}

/* PLAY touches the mixer, so the frame task answers it. */
CONSOLE_FRAME_SLOT(console_sound_slot, console_sound_args_t, true, answer)

static void
console_verb_sound(const char* args, console_reply_fn reply) {
    console_sound_args_t parsed;
    if (!console_sound_parse(args, &parsed)) {
        reply("SOUND_ERR usage: sound play <pack>/<name> [loop] [gain] | stop [voice|all] | stats | onset <event> [hz] "
              "[gain]");
        return;
    }
    if (parsed.action != CONSOLE_SOUND_PLAY) {
        sound_run_parsed(&parsed, reply);
        return;
    }
    if (!console_frame_post(console_frame_slots(), &console_sound_slot, &parsed)) {
        reply("SOUND_ERR play pending");
    }
}
CONSOLE_VERB_LONG(sound, CONSOLE_SOUND_LINE_MAX - sizeof("sound") - 1, CONSOLE_SOUND_LINE_MAX, console_verb_sound)
