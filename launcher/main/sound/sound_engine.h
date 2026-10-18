/* Shell, boot and development access. Applications use sound_play.h. */
#pragma once

#include "audio/audio_codec.h"
#include "sound/sound_mixer.h"
#include "sound/sound_status.h"

#define SOUND_MIXER_TASK_CORE            1
/* Preempts PRESENT_TASK_PRIORITY and JOB_CORE1_PRIORITY on core 1,
 * then blocks in I2S for most of each block. */
#define SOUND_MIXER_TASK_PRIORITY        6
/* A308 playback peak: 1,964 bytes, rounded up. Capture setup runs on the caller task. */
#define SOUND_MIXER_STACK_PLAYBACK_BYTES 2048
#define SOUND_MIXER_STACK_MARGIN_BYTES   512
#define SOUND_MIXER_TASK_STACK           (SOUND_MIXER_STACK_PLAYBACK_BYTES + SOUND_MIXER_STACK_MARGIN_BYTES)
/* One play and control call per voice fit between drains. */
#define SOUND_MIXER_QUEUE_DEPTH          16
#define SOUND_QUEUE_WAIT_TICKS           0
/* A reader exceeding this is stuck; abort rather than free memory it may still read. */
#define SOUND_EXIT_READ_TIMEOUT_MS       5000
/* A stalled mixer must not hold a capture caller indefinitely. */
#define SOUND_CAPTURE_TIMEOUT_MS         5000
/* Yield between barrier checks so the mixer can retire its current read. */
#define SOUND_CONTROL_POLL_MS            1
#define SOUND_MIXER_BLOCK_FRAMES         AUDIO_CODEC_BLOCK_SAMPLES

typedef struct {
    sound_control_t control;
    uint32_t positions[SOUND_VOICE_COUNT];
    sound_amp_state_t amp;
    size_t voice_table_bytes;
    size_t queue_bytes;
    size_t stack_bytes;
    size_t dma_sample_bytes;
    size_t mixer_bytes;
#if CONFIG_LAUNCHER_DEVELOPMENT
    size_t stack_min_free_bytes;
    size_t stack_margin_bytes;
#endif
} sound_stats_t;

bool sound_capture_open(void);
bool sound_capture_close(void);
bool sound_init(void);
void sound_begin_app(void);
void sound_end_app(void);
void sound_stop_app_voices(void);
#if CONFIG_LAUNCHER_DEVELOPMENT
void sound_get_stats(sound_stats_t* out);
#endif
sound_voice_t sound_play_tone(uint16_t hz, uint8_t gain_pct);
bool sound_voice_audible(sound_voice_t voice);
