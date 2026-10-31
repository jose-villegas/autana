/* The mixer task owns PCM writes; callers never wait for it (locking rules at control_lock). */
#include "sound/sound_engine.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "asset/asset_store.h"
#include "audio/audio_in.h"
#include "audio/audio_out_device.h"
#include "core/timing.h"
#include "core/tune.h"
#include "profile/frame_cost.h"

TUNE_OWNER(sound);
TUNE_HOT(sound, declick_us, SOUND_DECLICK_US, 0, MATH_US_PER_S);

static const char* TAG = "sound";
/* Covers handle reservation only. A nonblocking producer flag serializes zero-wait queue sends
 * outside it; decoding, mixing and I2S writes never take it. A finished voice retires only a
 * matching handle, so it cannot revive a stopped voice or overwrite a newer reservation. */
static portMUX_TYPE control_lock = portMUX_INITIALIZER_UNLOCKED;
static sound_control_t control;
static atomic_flag producer = ATOMIC_FLAG_INIT;
static sound_mixer_t mixer;
static QueueHandle_t commands;
static StaticQueue_t queue_control;
static uint8_t queue_storage[SOUND_MIXER_QUEUE_DEPTH * sizeof(sound_command_t)];
static const char* failure;
static bool attempted;
#if CONFIG_LAUNCHER_DEVELOPMENT
static size_t stack_min_free_bytes;
static sound_capture_t capture;

static struct {
    sound_amp_state_t amp;
    uint32_t positions[SOUND_VOICE_COUNT];
} render_snapshot;
#endif

static bool
begin_command(bool count_drop, uint32_t* generation) {
    portENTER_CRITICAL(&control_lock);
    *generation = control.app_generation;
    const bool ready = control.ready;
    portEXIT_CRITICAL(&control_lock);
    const bool acquired = !atomic_flag_test_and_set_explicit(&producer, memory_order_acquire);
    if (acquired && ready && uxQueueSpacesAvailable(commands) != 0) {
        return true;
    }
    if (ready && count_drop) {
        portENTER_CRITICAL(&control_lock);
        control.dropped_commands++;
        portEXIT_CRITICAL(&control_lock);
    }
    if (acquired) {
        atomic_flag_clear_explicit(&producer, memory_order_release);
    }
    return false;
}

static void
finish_command(const sound_command_t* command) {
    if (command) {
        const BaseType_t sent = xQueueSend(commands, command, SOUND_QUEUE_WAIT_TICKS);
        configASSERT(sent == pdTRUE);
        (void)sent;
    }
    atomic_flag_clear_explicit(&producer, memory_order_release);
}

static sound_voice_t
play_source(const sound_t* sound, const sound_play_opts_t* opts, uint16_t hz, uint16_t ms) {
    uint32_t generation;
    if (!begin_command(true, &generation)) {
        return SOUND_VOICE_NULL;
    }
    sound_command_t command;
    portENTER_CRITICAL(&control_lock);
    const sound_voice_t voice = sound_control_play(&control, sound, opts, hz, ms, &command, generation);
    portEXIT_CRITICAL(&control_lock);
    finish_command(voice.id ? &command : NULL);
    return voice;
}

sound_voice_t
sound_play(const sound_t* sound, const sound_play_opts_t* opts) {
    return play_source(sound, opts, 0, 0);
}

sound_voice_t
sound_play_tone(uint16_t hz, uint16_t ms, uint8_t gain_pct, sound_bus_t bus) {
    if (!audio_out_args_valid(hz, AUDIO_OUT_MAX_MS, gain_pct)) {
        return SOUND_VOICE_NULL;
    }
    const sound_play_opts_t opts = {.bus = bus, .gain_pct = gain_pct, .priority = SOUND_PRIORITY_MAX, .loop = true};
    return play_source(NULL, &opts, hz, ms);
}

static void
control_command(sound_voice_t voice, sound_bus_t bus, sound_command_kind_t kind, uint8_t gain) {
    uint32_t generation;
    if (!begin_command(true, &generation)) {
        return;
    }
    const bool voice_command = kind == SOUND_COMMAND_STOP || kind == SOUND_COMMAND_GAIN;
    sound_command_t command;
    portENTER_CRITICAL(&control_lock);
    const bool accepted = sound_control_admit(&control, generation)
                          && (voice_command ? sound_control_voice_command(&control, voice, kind, gain, &command)
                                            : sound_control_bus_command(&control, bus, kind, gain, &command));
    portEXIT_CRITICAL(&control_lock);
    finish_command(accepted ? &command : NULL);
}

void
sound_stop(sound_voice_t voice) {
    control_command(voice, SOUND_BUS_INVALID, SOUND_COMMAND_STOP, 0);
}

void
sound_set_gain(sound_voice_t voice, uint8_t pct) {
    control_command(voice, SOUND_BUS_INVALID, SOUND_COMMAND_GAIN, pct);
}

bool
sound_is_playing(sound_voice_t voice) {
    portENTER_CRITICAL(&control_lock);
    const bool playing = sound_control_is_playing(&control, voice);
    portEXIT_CRITICAL(&control_lock);
    return playing;
}

bool
sound_voice_audible(sound_voice_t voice) {
    portENTER_CRITICAL(&control_lock);
    const bool audible = sound_control_is_audible(&control, voice);
    portEXIT_CRITICAL(&control_lock);
    return audible;
}

sound_bus_t
sound_bus_create(const char* name) {
    uint32_t generation;
    if (!begin_command(true, &generation)) {
        return SOUND_BUS_INVALID;
    }
    sound_command_t command;
    portENTER_CRITICAL(&control_lock);
    const sound_bus_t bus = sound_control_bus_create(&control, name, &command, generation);
    portEXIT_CRITICAL(&control_lock);
    finish_command(bus.id != SOUND_BUS_INVALID.id ? &command : NULL);
    return bus;
}

void
sound_bus_destroy(sound_bus_t bus) {
    control_command(SOUND_VOICE_NULL, bus, SOUND_COMMAND_BUS_DESTROY, 0);
}

void
sound_set_bus_gain(sound_bus_t bus, uint8_t pct) {
    control_command(SOUND_VOICE_NULL, bus, SOUND_COMMAND_BUS_GAIN, pct);
}

void
sound_begin_app(void) {
    portENTER_CRITICAL(&control_lock);
    sound_control_begin_app(&control);
    portEXIT_CRITICAL(&control_lock);
}

void
sound_end_app(void) {
    portENTER_CRITICAL(&control_lock);
    sound_control_end_app(&control);
    portEXIT_CRITICAL(&control_lock);
}

void
sound_stop_app_voices(void) {
    portENTER_CRITICAL(&control_lock);
    if (!control.ready) {
        portEXIT_CRITICAL(&control_lock);
        return;
    }
    const uint32_t retired = control.app_generation;
    sound_control_exit(&control);
    portEXIT_CRITICAL(&control_lock);
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_EXIT_READ_TIMEOUT_MS * MATH_US_PER_MS);
    for (;;) {
        portENTER_CRITICAL(&control_lock);
        const bool pending = sound_control_read_pending(&control, retired);
        portEXIT_CRITICAL(&control_lock);
        if (!pending) {
            return;
        }
        if (timing_now_us() >= deadline) {
            ESP_LOGE(TAG, "app exit: borrowed sound read exceeded %u ms", (unsigned)SOUND_EXIT_READ_TIMEOUT_MS);
            abort();
        }
        timing_sleep_ms(SOUND_CONTROL_POLL_MS);
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
void
sound_get_stats(sound_stats_t* out) {
    portENTER_CRITICAL(&control_lock);
    *out = (sound_stats_t){.control = control,
                           .amp = render_snapshot.amp,
                           .voice_table_bytes = sizeof mixer.voices,
                           .queue_bytes = sizeof queue_storage + sizeof queue_control,
                           .stack_bytes = SOUND_MIXER_TASK_STACK,
                           .dma_sample_bytes = AUDIO_CODEC_DMA_BLOCKS * AUDIO_CODEC_BLOCK_SAMPLES * sizeof(int16_t),
                           .mixer_bytes = sizeof mixer + sizeof control};
    out->mixer_bytes += sizeof stack_min_free_bytes + sizeof render_snapshot + sizeof capture;
    out->stack_min_free_bytes = stack_min_free_bytes;
    out->stack_margin_bytes = SOUND_MIXER_STACK_MARGIN_BYTES;
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        out->positions[i] = render_snapshot.positions[i];
    }
    portEXIT_CRITICAL(&control_lock);
}

#endif

static void
fail(const char* step) {
    portENTER_CRITICAL(&control_lock);
    const bool first_failure = failure == NULL;
    if (first_failure) {
        failure = step;
    }
    control.ready = false;
#if CONFIG_LAUNCHER_DEVELOPMENT
    stack_min_free_bytes = 0;
#endif
    portEXIT_CRITICAL(&control_lock);
    if (first_failure) {
        ESP_LOGE(TAG, "audio: init failed (%s)", step);
    }
}

static void
drain_commands(void) {
    sound_command_t command;
    for (int n = 0; n < SOUND_MIXER_QUEUE_DEPTH; n++) {
        if (xQueueReceive(commands, &command, SOUND_QUEUE_WAIT_TICKS) != pdTRUE) {
            break;
        }
#if CONFIG_LAUNCHER_DEVELOPMENT
        if (command.kind == SOUND_COMMAND_CAPTURE_PARK) {
            portENTER_CRITICAL(&control_lock);
            const bool ok = sound_capture_park(&capture, command.app_generation);
            portEXIT_CRITICAL(&control_lock);
            (void)xTaskNotify(command.waiter, sound_capture_result(command.app_generation, ok), eSetValueWithOverwrite);
            for (;;) {
                portENTER_CRITICAL(&control_lock);
                const bool parked = sound_capture_is_parked(&capture, command.app_generation);
                portEXIT_CRITICAL(&control_lock);
                if (!parked) {
                    break;
                }
                timing_sleep_ms(SOUND_CONTROL_POLL_MS);
            }
        } else
#endif
        {
            portENTER_CRITICAL(&control_lock);
            const uint32_t generation = sound_control_begin_read(&control);
            portEXIT_CRITICAL(&control_lock);
            sound_mixer_apply(&mixer, &command, generation);
            portENTER_CRITICAL(&control_lock);
            sound_control_end_read(&control);
            portEXIT_CRITICAL(&control_lock);
        }
    }
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static void
publish_render_snapshot(size_t stack_free) {
    stack_min_free_bytes = stack_free;
    render_snapshot.amp = mixer.amp;
    for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
        render_snapshot.positions[i] = mixer.voices[i].tone_source ? 0 : mixer.voices[i].source.cursor.position;
    }
}
#endif

static void
apply_tuning(uint32_t* generation) {
    if (*generation == TUNE_GENERATION(sound)) {
        return;
    }
    const uint32_t frames = (uint32_t)((uint64_t)declick_us * AUDIO_OUT_SAMPLE_RATE_HZ / MATH_US_PER_S);
    mixer.declick_frames = declick_us != 0 && !frames ? 1 : frames;
    if (!mixer.declick_frames) {
        mixer.residual = 0;
    }
    *generation = TUNE_GENERATION(sound);
}

static void
mixer_task(void* unused) {
    int16_t out[SOUND_MIXER_BLOCK_FRAMES];
    int16_t scratch[SOUND_MIXER_BLOCK_FRAMES];
    int32_t accumulator[SOUND_MIXER_BLOCK_FRAMES];
    if (!audio_out_amp_enable(true)) {
        fail("amplifier");
        vTaskDelete(NULL);
        return;
    }
    mixer.amp_started_ms = (uint64_t)timing_now_us() / MATH_US_PER_MS;
    uint32_t tuning_generation = UINT32_MAX;
    for (;;) {
        apply_tuning(&tuning_generation);
        portENTER_CRITICAL(&control_lock);
        if (control.reset_gains) {
            const sound_command_t command = {.kind = SOUND_COMMAND_APP_EXIT};
            sound_mixer_apply(&mixer, &command, control.app_generation);
            control.reset_gains = false;
        }
        portEXIT_CRITICAL(&control_lock);
        drain_commands();
        portENTER_CRITICAL(&control_lock);
        sound_mixer_sync(&mixer, &control);
        (void)sound_control_begin_read(&control);
        portEXIT_CRITICAL(&control_lock);
        const int64_t start_us = timing_now_us();
        sound_mixer_render(&mixer, out, accumulator, scratch, SOUND_MIXER_BLOCK_FRAMES,
                           (uint64_t)start_us / MATH_US_PER_MS, AUDIO_OUT_AMP_STARTUP_MS);
#if FRAME_COST_ENABLED
        const int64_t mix_us = timing_now_us() - start_us;
#endif
        portENTER_CRITICAL(&control_lock);
        sound_control_end_read(&control);
        portEXIT_CRITICAL(&control_lock);
        _Static_assert(sizeof(SOUND_MIXER_FRAME_COST_NAME) - 1 <= FRAME_COST_NAME_MAX,
                       "mixer frame-cost name exceeds report capacity");
        FRAME_COST_EXTERNAL(SOUND_MIXER_FRAME_COST_NAME, mix_us);
#if CONFIG_LAUNCHER_DEVELOPMENT
        const size_t stack_free = uxTaskGetStackHighWaterMark(NULL);
#endif
        portENTER_CRITICAL(&control_lock);
        sound_control_complete(&control, &mixer);
#if CONFIG_LAUNCHER_DEVELOPMENT
        publish_render_snapshot(stack_free);
#endif
        portEXIT_CRITICAL(&control_lock);
        if (!audio_out_write(out, SOUND_MIXER_BLOCK_FRAMES)) {
            fail("I2S write");
            break;
        }
    }
    (void)audio_out_amp_enable(false);
    vTaskDelete(NULL);
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static bool
capture_request(uint32_t sequence) {
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    uint32_t result;
    (void)xTaskNotifyWait(0, UINT32_MAX, &result, 0);
    bool sent = false;
    const TaskHandle_t waiter = xTaskGetCurrentTaskHandle();
    do {
        portENTER_CRITICAL(&control_lock);
        const bool ready = control.ready;
        portEXIT_CRITICAL(&control_lock);
        uint32_t generation;
        if (begin_command(false, &generation)) {
            const sound_command_t command = {.app_generation = sequence,
                                             .bus = SOUND_BUS_SYSTEM,
                                             .waiter = waiter,
                                             .kind = SOUND_COMMAND_CAPTURE_PARK};
            finish_command(&command);
            sent = true;
        }
        if (!ready) {
            return false;
        }
        if (!sent) {
            timing_sleep_ms(SOUND_CONTROL_POLL_MS);
        }
    } while (!sent && timing_now_us() < deadline);
    while (sent && timing_now_us() < deadline) {
        const int64_t remaining_ms = (deadline - timing_now_us()) / MATH_US_PER_MS;
        if (remaining_ms <= 0 || xTaskNotifyWait(0, UINT32_MAX, &result, pdMS_TO_TICKS(remaining_ms)) != pdTRUE) {
            return false;
        }
        if (sound_capture_matches(result, sequence)) {
            return sound_capture_succeeded(result, sequence);
        }
    }
    return false;
}

static bool
capture_hold(bool (*during)(void)) {
    const int64_t deadline = timing_now_us() + ((int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS);
    uint32_t sequence = 0;
    sound_capture_acquire_result_t acquired;
    do {
        portENTER_CRITICAL(&control_lock);
        if (!control.ready) {
            portEXIT_CRITICAL(&control_lock);
            return during == NULL;
        }
        acquired = sound_capture_acquire(&capture, timing_now_us(), deadline, &sequence);
        portEXIT_CRITICAL(&control_lock);
        if (acquired == SOUND_CAPTURE_WAIT) {
            timing_sleep_ms(SOUND_CONTROL_POLL_MS);
        }
    } while (acquired == SOUND_CAPTURE_WAIT);
    const bool ok = acquired == SOUND_CAPTURE_ACQUIRED && capture_request(sequence) && (!during || during());
    if (sequence) {
        portENTER_CRITICAL(&control_lock);
        sound_capture_resume(&capture, sequence);
        portEXIT_CRITICAL(&control_lock);
    }
    return ok;
}

bool
sound_fence(void) {
    return capture_hold(NULL);
}

static bool
capture_close(void) {
    audio_in_close();
    return true;
}

bool
sound_capture_open(void) {
    return capture_hold(audio_in_open);
}

bool
sound_capture_close(void) {
    return capture_hold(capture_close);
}

#endif

bool
sound_init(void) {
    if (attempted) {
        return control.ready;
    }
    attempted = true;
    sound_control_init(&control, false);
    sound_mixer_init(&mixer);
    if (!audio_out_open()) {
        fail(audio_codec_failure() ? audio_codec_failure() : "output");
        return false;
    }
    commands = xQueueCreateStatic(SOUND_MIXER_QUEUE_DEPTH, sizeof(sound_command_t), queue_storage, &queue_control);
    if (!commands) {
        fail("command queue");
        return false;
    }
    control.ready = true;
    if (xTaskCreatePinnedToCore(mixer_task, "sound_mixer", SOUND_MIXER_TASK_STACK, NULL, SOUND_MIXER_TASK_PRIORITY,
                                NULL, SOUND_MIXER_TASK_CORE)
        != pdPASS) {
        fail("mixer task");
        return false;
    }
    return true;
}

const char*
sound_init_failure(void) {
    portENTER_CRITICAL(&control_lock);
    const char* step = failure;
    portEXIT_CRITICAL(&control_lock);
    return step;
}
