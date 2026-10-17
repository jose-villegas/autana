#include "sound/sound_engine.h"

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
#include "profile/frame_cost.h"

#define SOUND_QUEUE_TONE_FLAG   (1u << 6)
#define SOUND_QUEUE_LOOP_FLAG   (1u << 7)
#define SOUND_QUEUE_OPCODE_MASK (SOUND_QUEUE_TONE_FLAG - 1)

typedef struct {
    sound_voice_t voice;
    uint32_t app_generation;

    union {
        const sound_t* sound;
        uint32_t value;
        TaskHandle_t waiter;
    } payload;

    uint8_t opcode_flags;
    uint8_t bus;
    uint8_t gain_pct;
    uint8_t priority;
} sound_device_command_t;

_Static_assert(sizeof(sound_device_command_t) == SOUND_MIXER_COMMAND_BYTES, "queue command exceeds its RAM budget");
_Static_assert(SOUND_COMMAND_BUS_DESTROY <= SOUND_QUEUE_OPCODE_MASK, "opcode overlaps command flags");

static const asset_pack_t* engine_pack;
static const char* TAG = "sound";
static portMUX_TYPE control_lock = portMUX_INITIALIZER_UNLOCKED;
static sound_control_t control;
static sound_mixer_t mixer;
static uint32_t positions[SOUND_VOICE_COUNT];
static sound_amp_state_t amp_state;
static QueueHandle_t commands;
static StaticQueue_t queue_control;
static uint8_t queue_storage[SOUND_MIXER_QUEUE_DEPTH * sizeof(sound_device_command_t)];
static const char* failure;
static bool attempted;
#if CONFIG_LAUNCHER_DEVELOPMENT
static TaskHandle_t mixer_task_handle;
#endif

static bool
command_space(void) {
    if (!control.ready) {
        return false;
    }
    if (uxQueueSpacesAvailable(commands) == 0) {
        control.dropped_commands++;
        return false;
    }
    return true;
}

static void
submit(const sound_command_t* command, TaskHandle_t waiter) {
    sound_device_command_t queued = {
        .voice = command->voice,
        .app_generation = command->app_generation,
        .opcode_flags = (uint8_t)((unsigned)command->kind | (command->opts.loop ? SOUND_QUEUE_LOOP_FLAG : 0)),
        .bus = command->opts.bus.id,
        .gain_pct = command->opts.gain_pct,
        .priority = command->opts.priority};
    if (waiter) {
        queued.payload.waiter = waiter;
    } else if (command->tone_hz) {
        queued.opcode_flags |= SOUND_QUEUE_TONE_FLAG;
        queued.payload.value = command->tone_hz;
    } else {
        queued.payload.sound = command->sound;
    }
    /* Producers hold control_lock through reservation and send; the consumer only frees queue space. */
    const BaseType_t sent = xQueueSend(commands, &queued, 0);
    configASSERT(sent == pdTRUE);
    (void)sent;
}

static sound_voice_t
play_source(const sound_t* sound, const sound_play_opts_t* opts, uint16_t hz) {
    sound_voice_t voice = SOUND_VOICE_NULL;
    portENTER_CRITICAL(&control_lock);
    if (command_space()) {
        sound_command_t command;
        voice = sound_control_play(&control, sound, opts, hz, &command);
        if (voice.id) {
            submit(&command, NULL);
        }
    }
    portEXIT_CRITICAL(&control_lock);
    return voice;
}

sound_voice_t
sound_play(const sound_t* sound, const sound_play_opts_t* opts) {
    return play_source(sound, opts, 0);
}

sound_voice_t
sound_play_tone(uint16_t hz, uint8_t gain_pct) {
    if (!audio_out_args_valid(hz, AUDIO_OUT_MAX_MS, gain_pct)) {
        return SOUND_VOICE_NULL;
    }
    const sound_play_opts_t opts = {.bus = SOUND_BUS_SYSTEM, .gain_pct = gain_pct, .priority = UINT8_MAX, .loop = true};
    return play_source(NULL, &opts, hz);
}

static void
control_command(sound_voice_t voice, sound_bus_t bus, sound_command_kind_t kind, uint8_t gain) {
    const bool voice_command = kind == SOUND_COMMAND_STOP || kind == SOUND_COMMAND_GAIN;
    portENTER_CRITICAL(&control_lock);
    if (gain <= SOUND_GAIN_MAX_PCT && (!voice_command || sound_control_is_playing(&control, voice))
        && command_space()) {
        sound_command_t command;
        const bool accepted = voice_command ? sound_control_voice_command(&control, voice, kind, gain, &command)
                                            : sound_control_bus_command(&control, bus, kind, gain, &command);
        if (accepted) {
            submit(&command, NULL);
        }
    }
    portEXIT_CRITICAL(&control_lock);
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
    sound_bus_t bus = SOUND_BUS_INVALID;
    portENTER_CRITICAL(&control_lock);
    if (command_space()) {
        sound_command_t command;
        bus = sound_control_bus_create(&control, name, &command);
        if (bus.id != SOUND_BUS_INVALID.id) {
            submit(&command, NULL);
        }
    }
    portEXIT_CRITICAL(&control_lock);
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
sound_stop_app_voices(void) {
    portENTER_CRITICAL(&control_lock);
    if (!control.ready) {
        portEXIT_CRITICAL(&control_lock);
        return;
    }
    const uint32_t retired = control.app_generation;
    sound_control_exit(&control);
    portEXIT_CRITICAL(&control_lock);
    const int64_t deadline = timing_now_us() + (int64_t)SOUND_EXIT_READ_TIMEOUT_MS * MATH_US_PER_MS;
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

void
sound_get_stats(sound_stats_t* out) {
    portENTER_CRITICAL(&control_lock);
    *out = (sound_stats_t){.control = control,
                           .amp = amp_state,
                           .voice_table_bytes = sizeof mixer.voices,
                           .queue_bytes = sizeof queue_storage + sizeof queue_control,
                           .stack_bytes = SOUND_MIXER_TASK_STACK,
                           .dma_sample_bytes = AUDIO_CODEC_DMA_BLOCKS * AUDIO_CODEC_BLOCK_SAMPLES * sizeof(int16_t),
                           .mixer_bytes = sizeof mixer + sizeof control + sizeof positions};
#if CONFIG_LAUNCHER_DEVELOPMENT
    out->mixer_bytes += sizeof mixer_task_handle;
    out->stack_min_free_bytes = mixer_task_handle ? uxTaskGetStackHighWaterMark(mixer_task_handle) : 0;
    out->stack_margin_bytes = SOUND_MIXER_STACK_MARGIN_BYTES;
#endif
    memcpy(out->positions, positions, sizeof positions);
    portEXIT_CRITICAL(&control_lock);
}

static void
fail(const char* step) {
    portENTER_CRITICAL(&control_lock);
    const bool first_failure = failure == NULL;
    if (first_failure) {
        failure = step;
    }
    control.ready = false;
#if CONFIG_LAUNCHER_DEVELOPMENT
    mixer_task_handle = NULL;
#endif
    portEXIT_CRITICAL(&control_lock);
    if (first_failure) {
        ESP_LOGE(TAG, "audio: init failed (%s)", step);
    }
}

static sound_command_t
expand_command(const sound_device_command_t* queued) {
    sound_command_t command = {.kind = (sound_command_kind_t)(queued->opcode_flags & SOUND_QUEUE_OPCODE_MASK),
                               .voice = queued->voice,
                               .app_generation = queued->app_generation,
                               .opts = {.bus = {.id = queued->bus},
                                        .gain_pct = queued->gain_pct,
                                        .priority = queued->priority,
                                        .loop = (queued->opcode_flags & SOUND_QUEUE_LOOP_FLAG) != 0}};
    if (queued->opcode_flags & SOUND_QUEUE_TONE_FLAG) {
        command.tone_hz = (uint16_t)queued->payload.value;
    } else if (command.kind == SOUND_COMMAND_PLAY) {
        command.sound = queued->payload.sound;
    }
    return command;
}

static void
drain_commands(void) {
    sound_device_command_t queued;
    for (int n = 0; n < SOUND_MIXER_QUEUE_DEPTH; n++) {
        if (xQueueReceive(commands, &queued, 0) != pdTRUE) {
            break;
        }
        const sound_command_t command = expand_command(&queued);
        if (command.kind == SOUND_COMMAND_CAPTURE_OPEN || command.kind == SOUND_COMMAND_CAPTURE_CLOSE) {
            bool ok = true;
            if (command.kind == SOUND_COMMAND_CAPTURE_OPEN) {
                ok = audio_in_open();
            } else {
                audio_in_close();
            }
            (void)xTaskNotify(queued.payload.waiter, ok ? SOUND_CAPTURE_OK : SOUND_CAPTURE_FAILED,
                              eSetValueWithOverwrite);
        } else {
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

static void
mixer_task(void* unused) {
    int16_t out[SOUND_MIXER_BLOCK_FRAMES];
    int16_t scratch[SOUND_MIXER_BLOCK_FRAMES];
    int32_t accumulator[SOUND_MIXER_BLOCK_FRAMES];
    sound_amp_state_t previous_amp = SOUND_AMP_OFF;
    for (;;) {
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
        FRAME_COST_EXTERNAL("sound_mix", mix_us);
        if (mixer.amp != previous_amp) {
            if ((mixer.amp == SOUND_AMP_OFF || previous_amp == SOUND_AMP_OFF)
                && !audio_out_amp_enable(mixer.amp != SOUND_AMP_OFF)) {
                fail("amplifier");
                break;
            }
            previous_amp = mixer.amp;
        }
        portENTER_CRITICAL(&control_lock);
        sound_control_complete(&control, &mixer);
        amp_state = mixer.amp;
        for (int i = 0; i < SOUND_VOICE_COUNT; i++) {
            positions[i] = mixer.voices[i].tone_source ? 0 : mixer.voices[i].source.cursor.position;
        }
        portEXIT_CRITICAL(&control_lock);
        if (!audio_out_write(out, SOUND_MIXER_BLOCK_FRAMES)) {
            fail("I2S write");
            break;
        }
    }
    (void)audio_out_amp_enable(false);
    vTaskDelete(NULL);
}

static bool
capture_request(sound_command_kind_t kind) {
    const int64_t deadline = timing_now_us() + (int64_t)SOUND_CAPTURE_TIMEOUT_MS * MATH_US_PER_MS;
    uint32_t result;
    (void)xTaskNotifyWait(0, UINT32_MAX, &result, 0);
    bool sent = false;
    do {
        portENTER_CRITICAL(&control_lock);
        const bool ready = control.ready;
        if (command_space()) {
            const sound_command_t command = {.kind = kind, .opts = {.bus = SOUND_BUS_SYSTEM}};
            submit(&command, xTaskGetCurrentTaskHandle());
            sent = true;
        }
        portEXIT_CRITICAL(&control_lock);
        if (!ready) {
            return false;
        }
        if (!sent) {
            timing_sleep_ms(SOUND_CONTROL_POLL_MS);
        }
    } while (!sent && timing_now_us() < deadline);
    return sent && xTaskNotifyWait(0, UINT32_MAX, &result, pdMS_TO_TICKS(SOUND_CAPTURE_TIMEOUT_MS)) == pdTRUE
           && result == SOUND_CAPTURE_OK;
}

bool
sound_capture_open(void) {
    return capture_request(SOUND_COMMAND_CAPTURE_OPEN);
}

bool
sound_capture_close(void) {
    return capture_request(SOUND_COMMAND_CAPTURE_CLOSE);
}

asset_status_t
sound_open_engine(const char* name, sound_t* out) {
    return engine_pack ? sound_open(engine_pack, name, out) : ASSET_ERR_FORMAT;
}

bool
sound_init(void) {
    if (attempted) {
        return control.ready;
    }
    attempted = true;
    engine_pack = asset_store_pack(ASSET_ENGINE_PACK);
    sound_control_init(&control, false);
    sound_mixer_init(&mixer);
    if (!audio_out_open()) {
        fail(audio_codec_failure() ? audio_codec_failure() : "output");
        return false;
    }
    commands =
        xQueueCreateStatic(SOUND_MIXER_QUEUE_DEPTH, sizeof(sound_device_command_t), queue_storage, &queue_control);
    if (!commands) {
        fail("command queue");
        return false;
    }
    control.ready = true;
    TaskHandle_t* task_handle = NULL;
#if CONFIG_LAUNCHER_DEVELOPMENT
    task_handle = &mixer_task_handle;
#endif
    if (xTaskCreatePinnedToCore(mixer_task, "sound_mixer", SOUND_MIXER_TASK_STACK, NULL, SOUND_MIXER_TASK_PRIORITY,
                                task_handle, SOUND_MIXER_TASK_CORE)
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
