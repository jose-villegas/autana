/* Shell, boot and development access. Applications use sound_play.h. */
#pragma once

#include "audio/audio_codec.h"
#include "sound/sound_mixer.h"

#define SOUND_MIXER_TASK_CORE          1
/* Core 1 also runs present at 5 and the job worker at 3. The mixer preempts both, then blocks in I2S for most of each block. */
#define SOUND_MIXER_TASK_PRIORITY      6
#define SOUND_MIXER_STACK_BASE_BYTES   2048
#define SOUND_MIXER_STACK_MARGIN_BYTES 1024
#define SOUND_MIXER_TASK_STACK         (SOUND_MIXER_STACK_BASE_BYTES + SOUND_MIXER_STACK_MARGIN_BYTES)
/* A frame may enqueue one play and one control call per voice before the next drain. */
#define SOUND_MIXER_COMMAND_BYTES      16
#define SOUND_MIXER_QUEUE_DEPTH        16
#define SOUND_EXIT_READ_TIMEOUT_MS     5000
#define SOUND_CAPTURE_TIMEOUT_MS       5000
#define SOUND_CONTROL_POLL_MS          1
#define SOUND_CAPTURE_OK               1
#define SOUND_CAPTURE_FAILED           2
#define SOUND_MIXER_BLOCK_FRAMES       AUDIO_CODEC_BLOCK_SAMPLES

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
asset_status_t sound_open_engine(const char* name, sound_t* out);
bool sound_init(void);
const char* sound_init_failure(void);
void sound_stop_app_voices(void);
void sound_get_stats(sound_stats_t* out);
sound_voice_t sound_play_tone(uint16_t hz, uint8_t gain_pct);
bool sound_voice_audible(sound_voice_t voice);
