/* One codec lifetime with persistent transmit DMA and capture-owned receive DMA. */
#include "audio/audio_codec.h"
#include "audio/audio_codec_device.h"

#include <assert.h>

#include "bsp/esp32_s3_touch_amoled_1_8.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio/audio_codec_drc.h"
#include "audio/audio_codec_feedback.h"
#include "audio/audio_codec_mic.h"
#include "audio/audio_out.h"
#include "board/board.h"
#include "build/build_variant.h"
#include "profile/frame_watch.h"

#define AUDIO_CODEC_PROBE_TIMEOUT_MS 100
#define AUDIO_CODEC_PA_UNUSED        (-1)
#define AUDIO_CODEC_PA_SUPPLY_VOLTS  5.0F
#define AUDIO_CODEC_DAC_SUPPLY_VOLTS 3.3F

static const char* TAG = "audio_codec";
static i2s_chan_handle_t tx;
static i2s_chan_handle_t rx;
static const audio_codec_data_if_t* output_data;
static const audio_codec_data_if_t* input_data;
static const audio_codec_ctrl_if_t* ctrl;
static const audio_codec_gpio_if_t* gpio;
static const audio_codec_if_t* codec;
static esp_codec_dev_handle_t input_device;
static unsigned references;
static const char* failed_step;
static audio_codec_saved_t mic_saved;

static int
feedback_get(void* context, int reg, int* value) {
    (void)context;
    return codec->get_reg(codec, reg, value);
}

static int
feedback_set(void* context, int reg, int value) {
    (void)context;
    return codec->set_reg(codec, reg, value);
}

bool
audio_codec_mic_set(const audio_codec_mic_t* config) {
    return codec && config
           && audio_codec_check("microphone DSP",
                                audio_codec_mic_apply(&mic_saved, NULL, feedback_get, feedback_set, config)
                                    ? ESP_CODEC_DEV_OK
                                    : ESP_CODEC_DEV_DRV_ERR);
}

bool
audio_codec_mic_restore(void) {
    return codec ? audio_codec_check("restore microphone DSP", audio_codec_saved_restore(&mic_saved, NULL, feedback_set)
                                                                   ? ESP_CODEC_DEV_OK
                                                                   : ESP_CODEC_DEV_DRV_ERR)
                 : !mic_saved.restore_pending;
}

#if CONFIG_LAUNCHER_DEVELOPMENT
static audio_codec_feedback_t feedback;

bool
audio_codec_dac_feedback(bool on) {
    if (!codec || (on && !input_device)) {
        return false;
    }
    return audio_codec_check("DAC feedback", audio_codec_feedback_set(&feedback, NULL, feedback_get, feedback_set, on)
                                                 ? ESP_CODEC_DEV_OK
                                                 : ESP_CODEC_DEV_DRV_ERR);
}

bool
audio_codec_register(uint8_t reg, bool write, uint8_t* value) {
    if (!codec || !value) {
        return false;
    }
    if (write && !audio_codec_check("register write", codec->set_reg(codec, reg, *value))) {
        return false;
    }
    int readback;
    if (!audio_codec_check("register read", codec->get_reg(codec, reg, &readback))) {
        return false;
    }
    *value = (uint8_t)readback;
    return true;
}

#endif

bool
audio_codec_drc_set(const audio_codec_drc_t* config) {
    if (!codec || !config) {
        return false;
    }
    int old;
    return audio_codec_check("DRC ramp read", codec->get_reg(codec, AUDIO_CODEC_DRC_RAMP_REG, &old))
           && audio_codec_check(
               "DRC enable", codec->set_reg(codec, AUDIO_CODEC_DRC_ENABLE_REG, (int)audio_codec_drc_enable_reg(config)))
           && audio_codec_check(
               "DRC levels", codec->set_reg(codec, AUDIO_CODEC_DRC_LEVEL_REG, (int)audio_codec_drc_level_reg(config)))
           && audio_codec_check("DRC ramp", codec->set_reg(codec, AUDIO_CODEC_DRC_RAMP_REG,
                                                           (int)audio_codec_drc_ramp_reg((unsigned)old, config)));
}

const char*
audio_codec_failure(void) {
    return failed_step;
}

void
audio_codec_assert_not_frame_task(void) {
#if CONFIG_LAUNCHER_DEVELOPMENT
    assert(xTaskGetCurrentTaskHandle() != frame_watch_task());
#endif
}

bool
audio_codec_check(const char* operation, int status) {
    if (status == 0) {
        return true;
    }
    if (!failed_step) {
        failed_step = operation;
    }
    ESP_LOGE(TAG, "%s failed: %d", operation, status);
    return false;
}

static void
audio_codec_delete_channel(i2s_chan_handle_t* channel) {
    if (*channel) {
        i2s_chan_info_t info;
        if (audio_codec_check("I2S state", i2s_channel_get_info(*channel, &info)) && info.is_enabled) {
            (void)audio_codec_check("disable I2S", i2s_channel_disable(*channel));
        }
        (void)audio_codec_check("delete I2S", i2s_del_channel(*channel));
        *channel = NULL;
    }
}

static void
delete_input(void) {
    if (input_data) {
        (void)audio_codec_check("delete input data", audio_codec_delete_data_if(input_data));
        input_data = NULL;
    }
    audio_codec_delete_channel(&rx);
}

static void
audio_codec_destroy(void) {
    delete_input();
    if (output_data) {
        (void)audio_codec_check("delete output data", audio_codec_delete_data_if(output_data));
        output_data = NULL;
    }
    audio_codec_delete_channel(&tx);
    (void)audio_codec_check("delete codec", codec ? audio_codec_delete_codec_if(codec) : ESP_CODEC_DEV_OK);
    codec = NULL;
    (void)audio_codec_check("delete control", ctrl ? audio_codec_delete_ctrl_if(ctrl) : ESP_CODEC_DEV_OK);
    ctrl = NULL;
    (void)audio_codec_check("delete GPIO", gpio ? audio_codec_delete_gpio_if(gpio) : ESP_CODEC_DEV_OK);
    gpio = NULL;
    (void)audio_codec_check("amp off", board_audio_amp_enable(false));
}

static bool
init_channel(bool capture) {
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channels.auto_clear = true;
    channels.dma_desc_num = AUDIO_CODEC_DMA_BLOCKS;
    channels.dma_frame_num = AUDIO_CODEC_BLOCK_SAMPLES;
    if (!audio_codec_check(capture ? "new RX" : "new TX",
                           i2s_new_channel(&channels, capture ? NULL : &tx, capture ? &rx : NULL))) {
        return false;
    }
    const i2s_std_config_t format = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_OUT_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {.mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK, .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN},
    };
    const i2s_chan_handle_t channel = capture ? rx : tx;
    /* The IDF allocates DMA in init_std_mode, so an absent RX channel costs no DMA at boot. */
    if (!audio_codec_check(capture ? "init RX" : "init TX", i2s_channel_init_std_mode(channel, &format))) {
        return false;
    }
    return audio_codec_check(capture ? "enable RX" : "enable TX", i2s_channel_enable(channel));
}

static bool
audio_codec_init_data(bool capture) {
    if (!init_channel(capture)) {
        return false;
    }
    audio_codec_i2s_cfg_t config = {
        .port = CONFIG_BSP_I2S_NUM, .tx_handle = capture ? NULL : tx, .rx_handle = capture ? rx : NULL};
    const audio_codec_data_if_t* data = audio_codec_new_i2s_data(&config);
    if (capture) {
        input_data = data;
    } else {
        output_data = data;
    }
    return audio_codec_check(capture ? "new input data" : "new output data",
                             data ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM);
}

static bool
audio_codec_acquire(void) {
    if (references) {
        references++;
        return true;
    }
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!audio_codec_check("I2C bus", bus ? ESP_OK : ESP_ERR_INVALID_STATE)
        || !audio_codec_check("codec probe",
                              i2c_master_probe(bus, BOARD_AUDIO_CODEC_I2C_ADDR, AUDIO_CODEC_PROBE_TIMEOUT_MS))
        || !audio_codec_init_data(false)) {
        goto fail;
    }
    audio_codec_i2c_cfg_t ctrl_config = {.port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    ctrl = audio_codec_new_i2c_ctrl(&ctrl_config);
    if (!audio_codec_check("new control", ctrl ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM)) {
        goto fail;
    }
    gpio = audio_codec_new_gpio();
    if (!audio_codec_check("new GPIO", gpio ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM)) {
        goto fail;
    }
    es8311_codec_cfg_t codec_config = {
        .ctrl_if = ctrl,
        .gpio_if = gpio,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_CODEC_PA_UNUSED,
        .use_mclk = true,
        .hw_gain = {.pa_voltage = AUDIO_CODEC_PA_SUPPLY_VOLTS, .codec_dac_voltage = AUDIO_CODEC_DAC_SUPPLY_VOLTS}};
    codec = es8311_codec_new(&codec_config);
    if (!audio_codec_check("new codec", codec ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM)) {
        goto fail;
    }
    references = 1;
    return true;
fail:
    audio_codec_destroy();
    return false;
}

static void
audio_codec_release(void) {
    if (references && --references == 0) {
        audio_codec_destroy();
    }
}

static bool
init_capture_data(void) {
    /* RX creation and failure cleanup require TX paused on the S3. The mixer holds voice positions. */
    if (!audio_codec_check("pause TX for capture open", output_data->enable(output_data, ESP_CODEC_DEV_TYPE_OUT, false))
        || !audio_codec_init_data(true)) {
        delete_input();
        (void)audio_codec_check("resume TX after capture open",
                                output_data->enable(output_data, ESP_CODEC_DEV_TYPE_OUT, true));
        return false;
    }
    return true;
}

static void
release_failed_device(bool capture) {
    if (capture) {
        delete_input();
        (void)audio_codec_check("resume TX after capture open",
                                output_data->enable(output_data, ESP_CODEC_DEV_TYPE_OUT, true));
    }
    audio_codec_release();
}

esp_codec_dev_handle_t
audio_codec_device_open(esp_codec_dev_type_t type) {
    if (!audio_codec_acquire()) {
        return NULL;
    }
    const bool capture = type == ESP_CODEC_DEV_TYPE_IN;
    if (capture && !init_capture_data()) {
        audio_codec_release();
        return NULL;
    }
    esp_codec_dev_cfg_t config = {.dev_type = type, .codec_if = codec, .data_if = capture ? input_data : output_data};
    esp_codec_dev_handle_t device = esp_codec_dev_new(&config);
    if (!audio_codec_check("new device", device ? ESP_CODEC_DEV_OK : ESP_CODEC_DEV_NO_MEM)) {
        release_failed_device(capture);
        return NULL;
    }
    if (capture) {
        input_device = device;
    }
    if (capture
        && !audio_codec_check("resume TX after capture open",
                              output_data->enable(output_data, ESP_CODEC_DEV_TYPE_OUT, true))) {
        goto fail;
    }
    if (!audio_codec_check("retain codec", esp_codec_set_disable_when_closed(device, false))) {
        goto fail;
    }
    esp_codec_dev_sample_info_t format = {.sample_rate = AUDIO_OUT_SAMPLE_RATE_HZ,
                                          .bits_per_sample = AUDIO_CODEC_BITS_PER_SAMPLE,
                                          .channel = AUDIO_CODEC_CHANNELS};
    if (!audio_codec_check("open device", esp_codec_dev_open(device, &format))) {
        goto fail;
    }
    return device;
fail:
    audio_codec_device_close(device);
    return NULL;
}

void
audio_codec_device_close(esp_codec_dev_handle_t device) {
    if (!device) {
        return;
    }
    const bool capture = device == input_device;
    if (capture) {
        /* The S3 codec adapter defers RX shutdown while TX runs. This dev-capture gap is owned by the mixer. */
        (void)audio_codec_check("pause TX for capture close",
                                output_data->enable(output_data, ESP_CODEC_DEV_TYPE_OUT, false));
    }
    (void)audio_codec_check("close device", esp_codec_dev_close(device));
    esp_codec_dev_delete(device);
    if (capture) {
        input_device = NULL;
        delete_input();
        (void)audio_codec_check("resume TX after capture close",
                                output_data->enable(output_data, ESP_CODEC_DEV_TYPE_OUT, true));
    }
    audio_codec_release();
}
