/* Reference PCM renderer for bit-exact mixer output checks. */
#pragma once

#include <string.h>
#include "math/scalar/maths.h"
#include "sound/sound_mixer.h"

static size_t
oracle_read_voice(sound_mix_voice_t* voice, int16_t* out, size_t frames) {
    if (voice->tone_source) {
        audio_tone_fill(&voice->source.tone.oscillator, out, frames, AUDIO_OUT_VOLUME_MAX_PCT);
        return frames;
    }
    size_t total = 0;
    const uint32_t end = sound_end_frame(voice->source.cursor.sound);
    while (total < frames && voice->playing) {
        size_t count = frames - total;
        if (!voice->loop && count > end - voice->source.cursor.position) {
            count = end - voice->source.cursor.position;
        }
        const uint32_t read = sound_read(&voice->source.cursor, out + total, (uint32_t)count);
        total += read;
        if (!voice->loop) {
            voice->playing = read > 0 && voice->source.cursor.position < end;
        } else if (read < count || (!voice->source.cursor.sound->loop_end && voice->source.cursor.position == end)) {
            voice->playing = read > 0 && sound_seek(&voice->source.cursor, voice->source.cursor.sound, 0) == ASSET_OK;
        }
    }
    return total;
}

static bool
oracle_amp_ready(sound_mixer_t* mixer, uint64_t now_ms, uint32_t startup_ms) {
    if (mixer->amp == SOUND_AMP_WARMING && now_ms - mixer->amp_started_ms >= startup_ms) {
        mixer->amp = SOUND_AMP_ON;
    }
    return mixer->amp == SOUND_AMP_ON;
}

static void
oracle_add_tail(sound_mixer_t* mixer, int32_t* accumulator, size_t begin, size_t frames, int32_t residual) {
    if (!residual) {
        return;
    }
    const uint32_t coefficient = mixer->declick_frames > SOUND_GAIN_ONE ? 1 : SOUND_GAIN_ONE / mixer->declick_frames;
    for (size_t j = begin; j < frames && residual; j++) {
        accumulator[j] += residual;
        /* Round away from zero so a sub-LSB decay cannot leave a permanent DC tail. */
        const int64_t magnitude = residual < 0 ? -(int64_t)residual : residual;
        const int32_t decay = (int32_t)((magnitude * coefficient + SOUND_GAIN_ONE - 1) >> SOUND_GAIN_SHIFT);
        residual += residual < 0 ? decay : -decay;
    }
    mixer->residual += residual;
}

static bool
oracle_voice_at_cut(const sound_mix_voice_t* voice) {
    return !voice->tone_source && !voice->playing;
}

static void
oracle_mix_voice(sound_mixer_t* mixer, sound_mix_voice_t* voice, int32_t* accumulator, int16_t* scratch, size_t frames,
                 int32_t factor) {
    const size_t count = oracle_read_voice(voice, scratch, frames);
    if (mixer->declick_frames && count) {
        if (voice->first_render) {
            oracle_add_tail(mixer, accumulator, 0, frames, -((scratch[0] * factor) >> SOUND_GAIN_SHIFT));
            voice->first_render = false;
        }
        voice->last_sample = (scratch[count - 1] * factor) >> SOUND_GAIN_SHIFT;
    }
    for (size_t j = 0; j < count; j++) {
        accumulator[j] += (scratch[j] * factor) >> SOUND_GAIN_SHIFT;
    }
    if (mixer->declick_frames && oracle_voice_at_cut(voice)) {
        oracle_add_tail(mixer, accumulator, count, frames, voice->last_sample);
        voice->last_sample = 0;
        voice->first_render = true;
    }
}

static void
oracle_sound_mixer_render(sound_mixer_t* mixer, int16_t* out, int32_t* accumulator, int16_t* scratch, size_t frames,
                          uint64_t now_ms, uint32_t startup_ms) {
    memset(out, 0, frames * sizeof *out);
    if (!oracle_amp_ready(mixer, now_ms, startup_ms)) {
        return;
    }
    uint32_t factors[SOUND_BUS_COUNT_MAX];
    for (int i = 0; i < SOUND_BUS_COUNT_MAX; i++) {
        factors[i] = (uint32_t)mixer->bus_gain[i] * mixer->master_gain;
    }
    memset(accumulator, 0, frames * sizeof *accumulator);
    if (mixer->declick_frames) {
        const int32_t residual = mixer->residual;
        mixer->residual = 0;
        oracle_add_tail(mixer, accumulator, 0, frames, residual);
    } else {
        mixer->residual = 0;
    }
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        sound_mix_voice_t* voice = &mixer->voices[i];
        if (!voice->playing) {
            continue;
        }
        const uint32_t gain = (uint32_t)voice->gain_pct * factors[voice->bus.id];
        const int32_t factor =
            (int32_t)(((uint64_t)gain * SOUND_GAIN_ONE) / (SOUND_GAIN_FACTOR_SCALE * SOUND_GAIN_MAX_PCT));
        oracle_mix_voice(mixer, voice, accumulator, scratch, frames, factor);
    }
    for (size_t i = 0; i < frames; i++) {
        out[i] = maths_saturate(accumulator[i]);
    }
}
