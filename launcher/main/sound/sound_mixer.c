#include "sound/sound_mixer.h"

#include <string.h>

#include "math/scalar/mathi.h"
#include "math/scalar/maths.h"

uint32_t
sound_capture_begin(sound_capture_t* capture) {
    if (capture->requested) {
        return 0;
    }
    capture->sequence = (capture->sequence % SOUND_CAPTURE_SEQUENCE_MAX) + 1;
    capture->requested = capture->sequence;
    return capture->sequence;
}

sound_capture_acquire_result_t
sound_capture_acquire(sound_capture_t* capture, int64_t now_us, int64_t deadline_us, uint32_t* sequence) {
    *sequence = 0;
    if (now_us >= deadline_us) {
        return SOUND_CAPTURE_TIMED_OUT;
    }
    *sequence = sound_capture_begin(capture);
    return *sequence ? SOUND_CAPTURE_ACQUIRED : SOUND_CAPTURE_WAIT;
}

bool
sound_capture_park(sound_capture_t* capture, uint32_t sequence) {
    if (!sequence || capture->requested != sequence) {
        return false;
    }
    capture->parked = sequence;
    return true;
}

bool
sound_capture_is_parked(const sound_capture_t* capture, uint32_t sequence) {
    return sequence && capture->parked == sequence;
}

void
sound_capture_resume(sound_capture_t* capture, uint32_t sequence) {
    if (capture->requested == sequence) {
        capture->requested = 0;
        capture->parked = 0;
    }
}

uint32_t
sound_capture_result(uint32_t sequence, bool ok) {
    return (sequence << SOUND_CAPTURE_RESULT_BITS) | (ok ? SOUND_CAPTURE_OK : SOUND_CAPTURE_FAILED);
}

bool
sound_capture_matches(uint32_t result, uint32_t sequence) {
    return sequence && (result >> SOUND_CAPTURE_RESULT_BITS) == sequence;
}

bool
sound_capture_succeeded(uint32_t result, uint32_t sequence) {
    return sound_capture_matches(result, sequence) && (result & SOUND_CAPTURE_OK) != 0;
}

static bool
control_open(const sound_control_t* control) {
    return control->ready && control->app_generation != SOUND_APP_GENERATION_NONE;
}

bool
sound_control_admit(const sound_control_t* control, uint32_t generation) {
    return control_open(control) && control->app_generation == generation;
}

static int
voice_slot(sound_voice_t voice) {
    return voice.id == 0 ? -1 : (int)((voice.id - 1) % SOUND_VOICE_COUNT);
}

static sound_control_voice_t*
control_voice(sound_control_t* control, sound_voice_t voice) {
    const int slot = voice_slot(voice);
    if (!control->ready || slot < 0 || control->voices[slot].handle.id != voice.id || !control->voices[slot].playing) {
        return NULL;
    }
    return &control->voices[slot];
}

void
sound_control_init(sound_control_t* control, bool ready) {
    *control = (sound_control_t){
        .ready = ready, .app_generation = SOUND_APP_GENERATION_ENGINE, .next_generation = SOUND_APP_GENERATION_FIRST};
    static const char* const names[] = {"SYSTEM", "SFX", "MUSIC", "HAPTIC"};
    _Static_assert(sizeof names / sizeof names[0] == SOUND_BUS_BUILTIN_COUNT, "built-in bus names");
    for (int i = 0; i < SOUND_BUS_BUILTIN_COUNT; i++) {
        control->buses[i].used = true;
        control->buses[i].owner = SOUND_APP_GENERATION_ENGINE;
        control->buses[i].gain_pct = SOUND_GAIN_MAX_PCT;
        (void)strcpy(control->buses[i].name, names[i]);
    }
}

static int
choose_voice_slot(const sound_control_t* control) {
    int slot = 0;
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        const sound_control_voice_t* candidate = &control->voices[i];
        const sound_control_voice_t* chosen = &control->voices[slot];
        if (!candidate->playing) {
            slot = i;
            break;
        }
        if (candidate->opts.priority < chosen->opts.priority
            || (candidate->opts.priority == chosen->opts.priority && candidate->started < chosen->started)) {
            slot = i;
        }
    }
    return slot;
}

sound_voice_t
sound_control_play(sound_control_t* control, const sound_t* sound, const sound_play_opts_t* opts, uint16_t tone_hz,
                   uint16_t tone_ms, sound_command_t* command, uint32_t admitted_generation) {
    if (!sound_control_admit(control, admitted_generation)) {
        return SOUND_VOICE_NULL;
    }
    const sound_play_opts_t options = opts ? *opts
                                           : (sound_play_opts_t){.bus = SOUND_BUS_SFX,
                                                                 .gain_pct = SOUND_GAIN_MAX_PCT,
                                                                 .priority = SOUND_PRIORITY_NORMAL};
    if ((!tone_hz && (!sound || !sound->data || !sound->frames)) || options.bus.id >= SOUND_BUS_COUNT_MAX
        || !control->buses[options.bus.id].used || options.gain_pct > SOUND_GAIN_MAX_PCT) {
        return SOUND_VOICE_NULL;
    }
    const int slot = choose_voice_slot(control);
    sound_control_voice_t* voice = &control->voices[slot];
    const uint32_t generation =
        (voice->handle.id ? ((voice->handle.id - 1) / SOUND_VOICE_COUNT) + 1 : 0) % SOUND_VOICE_GENERATIONS;
    const sound_voice_t handle = {.id = (generation * SOUND_VOICE_COUNT) + (uint32_t)slot + 1};
    *voice = (sound_control_voice_t){.handle = handle,
                                     .started = ++control->starts,
                                     .opts = options,
                                     .owner = control->app_generation,
                                     .playing = true};
    *command = (sound_command_t){.kind = SOUND_COMMAND_PLAY,
                                 .app_generation = control->app_generation,
                                 .voice = handle,
                                 .bus = options.bus,
                                 .gain_pct = options.gain_pct,
                                 .priority = options.priority,
                                 .loop = options.loop};
    command->tone_source = tone_hz != 0;
    if (tone_hz) {
        command->tone.hz = tone_hz;
        command->tone.ms = tone_ms;
    } else {
        command->sound = sound;
    }
    return handle;
}

bool
sound_control_voice_command(sound_control_t* control, sound_voice_t handle, sound_command_kind_t kind, uint8_t gain,
                            sound_command_t* command) {
    sound_control_voice_t* voice = control_voice(control, handle);
    if (!control_open(control) || !voice || gain > SOUND_GAIN_MAX_PCT) {
        return false;
    }
    *command = (sound_command_t){
        .kind = kind, .voice = handle, .bus = voice->opts.bus, .app_generation = control->app_generation};
    command->gain_pct = gain;
    if (kind == SOUND_COMMAND_STOP) {
        voice->playing = false;
        voice->audible = false;
    } else if (kind == SOUND_COMMAND_GAIN) {
        voice->opts.gain_pct = gain;
    }
    return true;
}

bool
sound_control_is_playing(const sound_control_t* control, sound_voice_t voice) {
    const int slot = voice_slot(voice);
    return control->ready && slot >= 0 && control->voices[slot].playing && control->voices[slot].handle.id == voice.id;
}

bool
sound_control_is_audible(const sound_control_t* control, sound_voice_t voice) {
    return sound_control_is_playing(control, voice) && control->voices[voice_slot(voice)].audible;
}

sound_bus_t
sound_control_bus_create(sound_control_t* control, const char* name, sound_command_t* command, uint32_t generation) {
    if (!sound_control_admit(control, generation) || !name) {
        return SOUND_BUS_INVALID;
    }
    size_t length = 0;
    while (length < SOUND_BUS_NAME_MAX && name[length]) {
        length++;
    }
    if (!length || length == SOUND_BUS_NAME_MAX) {
        return SOUND_BUS_INVALID;
    }
    for (int i = SOUND_BUS_BUILTIN_COUNT; i < SOUND_BUS_COUNT_MAX; i++) {
        if (!control->buses[i].used) {
            sound_bus_state_t* bus = &control->buses[i];
            *bus = (sound_bus_state_t){.used = true, .gain_pct = SOUND_GAIN_MAX_PCT, .owner = control->app_generation};
            memcpy(bus->name, name, length + 1);
            *command = (sound_command_t){
                .kind = SOUND_COMMAND_BUS_CREATE, .app_generation = control->app_generation, .bus = {.id = (uint8_t)i}};
            return (sound_bus_t){.id = (uint8_t)i};
        }
    }
    return SOUND_BUS_INVALID;
}

bool
sound_control_bus_command(sound_control_t* control, sound_bus_t bus, sound_command_kind_t kind, uint8_t gain,
                          sound_command_t* command) {
    if (!control_open(control) || gain > SOUND_GAIN_MAX_PCT
        || (bus.id != SOUND_BUS_MASTER.id && (bus.id >= SOUND_BUS_COUNT_MAX || !control->buses[bus.id].used))
        || (kind == SOUND_COMMAND_BUS_DESTROY && (bus.id < SOUND_BUS_BUILTIN_COUNT || bus.id >= SOUND_BUS_COUNT_MAX))) {
        return false;
    }
    *command = (sound_command_t){.kind = kind, .app_generation = control->app_generation, .bus = bus, .gain_pct = gain};
    if (kind == SOUND_COMMAND_BUS_DESTROY) {
        control->buses[bus.id].used = false;
        for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
            if (control->voices[i].opts.bus.id == bus.id) {
                control->voices[i].playing = false;
                control->voices[i].audible = false;
            }
        }
    } else if (bus.id < SOUND_BUS_COUNT_MAX) {
        control->buses[bus.id].gain_pct = gain;
    }
    return true;
}

void
sound_control_exit(sound_control_t* control) {
    if (!control->ready) {
        return;
    }
    const uint32_t retired = control->app_generation;
    control->app_generation = SOUND_APP_GENERATION_NONE;
    control->reset_gains = true;
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        if (control->voices[i].owner == retired && retired != SOUND_APP_GENERATION_ENGINE) {
            control->voices[i].playing = false;
            control->voices[i].audible = false;
        }
    }
    for (int i = SOUND_BUS_BUILTIN_COUNT; i < SOUND_BUS_COUNT_MAX; i++) {
        if (control->buses[i].owner == retired && retired != SOUND_APP_GENERATION_ENGINE) {
            control->buses[i].used = false;
        }
    }
    for (int i = 1; i < SOUND_BUS_BUILTIN_COUNT; i++) {
        control->buses[i].gain_pct = SOUND_GAIN_MAX_PCT;
    }
}

void
sound_control_begin_app(sound_control_t* control) {
    control->app_generation = control->next_generation++;
    if (control->next_generation < SOUND_APP_GENERATION_FIRST) {
        control->next_generation = SOUND_APP_GENERATION_FIRST;
    }
}

void
sound_control_end_app(sound_control_t* control) {
    sound_control_exit(control);
    control->app_generation = SOUND_APP_GENERATION_ENGINE;
}

uint32_t
sound_control_begin_read(sound_control_t* control) {
    if (!control->ready) {
        return SOUND_APP_GENERATION_NONE;
    }
    control->read_generation = control->app_generation;
    return control->read_generation;
}

void
sound_control_end_read(sound_control_t* control) {
    control->read_generation = SOUND_APP_GENERATION_NONE;
}

bool
sound_control_read_pending(const sound_control_t* control, uint32_t generation) {
    return control->ready && generation != SOUND_APP_GENERATION_NONE && control->read_generation == generation;
}

void
sound_control_complete(sound_control_t* control, const sound_mixer_t* mixer) {
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        if (control->voices[i].handle.id == mixer->voices[i].handle.id) {
            control->voices[i].playing &= mixer->voices[i].playing;
            control->voices[i].audible = control->voices[i].playing && mixer->amp == SOUND_AMP_ON;
        }
    }
}

void
sound_mixer_init(sound_mixer_t* mixer) {
    *mixer = (sound_mixer_t){.amp = SOUND_AMP_WARMING, .master_gain = SOUND_GAIN_MAX_PCT};
    for (int i = 0; i < SOUND_BUS_COUNT_MAX; i++) {
        mixer->bus_gain[i] = SOUND_GAIN_MAX_PCT;
    }
}

static void
cut_voice(sound_mixer_t* mixer, sound_mix_voice_t* voice) {
    if (mixer->declick_frames && voice->playing) {
        mixer->residual += voice->last_sample;
    }
    voice->playing = false;
    voice->last_sample = 0;
}

static bool
apply_bus_command(sound_mixer_t* mixer, const sound_command_t* command) {
    const int bus = command->bus.id;
    if (command->kind == SOUND_COMMAND_BUS_GAIN) {
        if (bus == SOUND_BUS_MASTER.id) {
            mixer->master_gain = command->gain_pct;
        } else if (bus < SOUND_BUS_COUNT_MAX) {
            mixer->bus_gain[bus] = command->gain_pct;
        }
        return true;
    }
    if (command->kind == SOUND_COMMAND_BUS_CREATE) {
        mixer->bus_gain[bus] = SOUND_GAIN_MAX_PCT;
        return true;
    }
    if (command->kind == SOUND_COMMAND_BUS_DESTROY) {
        for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
            if (mixer->voices[i].bus.id == bus) {
                cut_voice(mixer, &mixer->voices[i]);
            }
        }
        return true;
    }
    return false;
}

static void
start_voice(sound_mix_voice_t* voice, const sound_command_t* command) {
    *voice = (sound_mix_voice_t){.handle = command->voice,
                                 .bus = command->bus,
                                 .gain_pct = command->gain_pct,
                                 .loop = command->loop,
                                 .tone_source = command->tone_source,
                                 .playing = true,
                                 .first_render = true};
    if (voice->tone_source) {
        audio_tone_init(&voice->source.tone.oscillator, command->tone.hz);
        voice->source.tone.frames_left = (uint32_t)audio_out_samples(command->tone.ms);
    } else if (sound_seek(&voice->source.cursor, command->sound, 0) != ASSET_OK) {
        voice->playing = false;
    }
}

void
sound_mixer_apply(sound_mixer_t* mixer, const sound_command_t* command, uint32_t app_generation) {
    if (command->kind == SOUND_COMMAND_APP_EXIT) {
        mixer->master_gain = SOUND_GAIN_MAX_PCT;
        for (int i = 1; i < SOUND_BUS_BUILTIN_COUNT; i++) {
            mixer->bus_gain[i] = SOUND_GAIN_MAX_PCT;
        }
        return;
    }
    if (command->app_generation == SOUND_APP_GENERATION_NONE
        || (command->app_generation != SOUND_APP_GENERATION_ENGINE && command->app_generation != app_generation)) {
        return;
    }
    if (apply_bus_command(mixer, command)) {
        return;
    }
    const int slot = voice_slot(command->voice);
    if (slot < 0) {
        return;
    }
    sound_mix_voice_t* voice = &mixer->voices[slot];
    if (command->kind == SOUND_COMMAND_PLAY) {
        cut_voice(mixer, voice);
        start_voice(voice, command);
    } else if (voice->handle.id == command->voice.id) {
        if (command->kind == SOUND_COMMAND_STOP) {
            cut_voice(mixer, voice);
        } else if (command->kind == SOUND_COMMAND_GAIN) {
            voice->gain_pct = command->gain_pct;
        }
    }
}

void
sound_mixer_sync(sound_mixer_t* mixer, const sound_control_t* control) {
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        if (!control->voices[i].playing || control->voices[i].handle.id != mixer->voices[i].handle.id) {
            cut_voice(mixer, &mixer->voices[i]);
        }
    }
}

/* Tones are rare, so their render stays in flash. */
static __attribute__((noinline)) size_t
read_tone(sound_mix_voice_t* voice, int16_t* out, size_t frames) {
    if (voice->source.tone.frames_left) {
        frames = frames > voice->source.tone.frames_left ? voice->source.tone.frames_left : frames;
        voice->source.tone.frames_left -= (uint32_t)frames;
        voice->playing = voice->source.tone.frames_left != 0;
    }
    audio_tone_fill(&voice->source.tone.oscillator, out, frames, AUDIO_OUT_VOLUME_MAX_PCT);
    return frames;
}

static inline size_t AUDIO_IRAM_ATTR
read_voice(sound_mix_voice_t* voice, int16_t* out, size_t frames) {
    if (voice->tone_source) {
        return read_tone(voice, out, frames);
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

static bool AUDIO_IRAM_ATTR
amp_ready(sound_mixer_t* mixer, uint64_t now_ms, uint32_t startup_ms) {
    if (mixer->amp == SOUND_AMP_WARMING && now_ms - mixer->amp_started_ms >= startup_ms) {
        mixer->amp = SOUND_AMP_ON;
    }
    return mixer->amp == SOUND_AMP_ON;
}

static void AUDIO_IRAM_ATTR
add_tail(sound_mixer_t* mixer, int32_t* accumulator, size_t begin, size_t frames, int32_t residual) {
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

static void AUDIO_IRAM_ATTR
mix_voice(sound_mixer_t* mixer, sound_mix_voice_t* voice, int32_t* accumulator, int16_t* scratch, size_t frames,
          int32_t factor) {
    const size_t count = read_voice(voice, scratch, frames);
    if (mixer->declick_frames && count) {
        if (voice->first_render) {
            add_tail(mixer, accumulator, 0, frames, -((scratch[0] * factor) >> SOUND_GAIN_SHIFT));
            voice->first_render = false;
        }
        voice->last_sample = (scratch[count - 1] * factor) >> SOUND_GAIN_SHIFT;
    }
    if (factor == SOUND_GAIN_ONE) {
        for (size_t j = 0; j < count; j++) {
            accumulator[j] += scratch[j];
        }
    } else {
        for (size_t j = 0; j < count; j++) {
            accumulator[j] += (scratch[j] * factor) >> SOUND_GAIN_SHIFT;
        }
    }
    if (mixer->declick_frames && !voice->playing) {
        add_tail(mixer, accumulator, count, frames, voice->last_sample);
        voice->last_sample = 0;
        voice->first_render = true;
    }
}

static bool
voice_ends_within(const sound_mix_voice_t* voice, size_t frames) {
    if (voice->tone_source) {
        return voice->source.tone.frames_left && frames >= voice->source.tone.frames_left;
    }
    const uint32_t end = sound_end_frame(voice->source.cursor.sound);
    return voice->source.cursor.position >= end || (!voice->loop && frames >= end - voice->source.cursor.position);
}

static sound_mix_voice_t* AUDIO_IRAM_ATTR
direct_voice(sound_mixer_t* mixer, size_t frames) {
    if (!frames || mixer->residual || mixer->master_gain != SOUND_GAIN_MAX_PCT) {
        return NULL;
    }
    sound_mix_voice_t* single = NULL;
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        sound_mix_voice_t* voice = &mixer->voices[i];
        if (!voice->playing) {
            continue;
        }
        if (single || voice->gain_pct != SOUND_GAIN_MAX_PCT || mixer->bus_gain[voice->bus.id] != SOUND_GAIN_MAX_PCT
            || (mixer->declick_frames && voice->first_render) || voice_ends_within(voice, frames)) {
            return NULL;
        }
        single = voice;
    }
    return single;
}

void AUDIO_IRAM_ATTR
sound_mixer_render(sound_mixer_t* mixer, int16_t* out, int32_t* accumulator, int16_t* scratch, size_t frames,
                   uint64_t now_ms, uint32_t startup_ms) {
    if (!amp_ready(mixer, now_ms, startup_ms)) {
        memset(out, 0, frames * sizeof *out);
        return;
    }
    sound_mix_voice_t* single = direct_voice(mixer, frames);
    if (single) {
        (void)read_voice(single, out, frames);
        if (mixer->declick_frames) {
            single->last_sample = out[frames - 1];
        }
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
        add_tail(mixer, accumulator, 0, frames, residual);
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
        mix_voice(mixer, voice, accumulator, scratch, frames, factor);
    }
    for (size_t i = 0; i < frames; i++) {
        out[i] = maths_saturate(accumulator[i]);
    }
}
