/* Codec interfaces and duplex I2S channels share one reference-counted lifetime. */
#include "audio/audio_codec.h"

#include <assert.h>

#include "audio/audio_out.h"
#include "board/board.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "profile/frame_watch.h"

#define AUDIO_CODEC_PROBE_TIMEOUT_MS 100
#define AUDIO_CODEC_PA_UNUSED        (-1)
#define AUDIO_CODEC_PA_SUPPLY_VOLTS  5.0F
#define AUDIO_CODEC_DAC_SUPPLY_VOLTS 3.3F

static const char* TAG = "audio_codec";
static i2s_chan_handle_t tx;
static i2s_chan_handle_t rx;
static const audio_codec_data_if_t* data;
static const audio_codec_ctrl_if_t* ctrl;
static const audio_codec_gpio_if_t* gpio;
static const audio_codec_if_t* codec;
static unsigned references;
static bool device_took_channels;
static bool tx_enabled;
static bool rx_enabled;

void
audio_codec_assert_not_frame_task(void) {
    assert(xTaskGetCurrentTaskHandle() != frame_watch_task());
}

bool
audio_codec_check(const char* operation, int status) {
    if (status == 0) {
        return true;
    }
    ESP_LOGE(TAG, "%s failed: %d", operation, status);
    return false;
}

static void
audio_codec_delete_channel(i2s_chan_handle_t* channel, const char* name) {
    if (*channel) {
        (void)audio_codec_check(name, i2s_del_channel(*channel));
        *channel = NULL;
    }
}

static void
audio_codec_destroy(void) {
    (void)audio_codec_check("delete codec", codec ? audio_codec_delete_codec_if(codec) : ESP_CODEC_DEV_OK);
    codec = NULL;
    (void)audio_codec_check("delete control", ctrl ? audio_codec_delete_ctrl_if(ctrl) : ESP_CODEC_DEV_OK);
    ctrl = NULL;
    (void)audio_codec_check("delete GPIO", gpio ? audio_codec_delete_gpio_if(gpio) : ESP_CODEC_DEV_OK);
    gpio = NULL;
    (void)audio_codec_check("delete data", data ? audio_codec_delete_data_if(data) : ESP_CODEC_DEV_OK);
    data = NULL;
    /* esp_codec_dev expects the channels enabled when it formats them, and its device close disables both.
     * Disable here only if no device ever took them over. */
    if (!device_took_channels) {
        if (tx_enabled) {
            (void)audio_codec_check("disable TX", i2s_channel_disable(tx));
        }
        if (rx_enabled) {
            (void)audio_codec_check("disable RX", i2s_channel_disable(rx));
        }
    }
    device_took_channels = false;
    tx_enabled = false;
    rx_enabled = false;
    audio_codec_delete_channel(&tx, "delete TX");
    audio_codec_delete_channel(&rx, "delete RX");
    (void)audio_codec_check("amp off", board_audio_amp_enable(false));
}

static bool
audio_codec_init_data(void) {
    i2s_chan_config_t channels = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM, I2S_ROLE_MASTER);
    channels.auto_clear = true;
    channels.dma_desc_num = AUDIO_CODEC_DMA_BLOCKS;
    channels.dma_frame_num = AUDIO_CODEC_BLOCK_SAMPLES;
    if (!audio_codec_check("new I2S", i2s_new_channel(&channels, &tx, &rx))) {
        return false;
    }
    const i2s_std_config_t format = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_OUT_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg =
            {.mclk = BSP_I2S_MCLK, .bclk = BSP_I2S_SCLK, .ws = BSP_I2S_LCLK, .dout = BSP_I2S_DOUT, .din = BSP_I2S_DSIN},
    };
    if (!audio_codec_check("init TX", i2s_channel_init_std_mode(tx, &format))
        || !audio_codec_check("init RX", i2s_channel_init_std_mode(rx, &format))) {
        return false;
    }
    tx_enabled = audio_codec_check("enable TX", i2s_channel_enable(tx));
    if (!tx_enabled) {
        return false;
    }
    rx_enabled = audio_codec_check("enable RX", i2s_channel_enable(rx));
    if (!rx_enabled) {
        return false;
    }
    audio_codec_i2s_cfg_t data_config = {.port = CONFIG_BSP_I2S_NUM, .tx_handle = tx, .rx_handle = rx};
    data = audio_codec_new_i2s_data(&data_config);
    if (!data) {
        ESP_LOGE(TAG, "new data interface failed");
        return false;
    }
    return true;
}

static bool
audio_codec_acquire(void) {
    if (references) {
        references++;
        return true;
    }
    const i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGE(TAG, "I2C bus unavailable");
        goto fail;
    }
    if (!audio_codec_check("codec probe",
                           i2c_master_probe(bus, BOARD_AUDIO_CODEC_I2C_ADDR, AUDIO_CODEC_PROBE_TIMEOUT_MS))) {
        goto fail;
    }
    if (!audio_codec_init_data()) {
        goto fail;
    }
    audio_codec_i2c_cfg_t ctrl_config = {.port = BSP_I2C_NUM, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    ctrl = audio_codec_new_i2c_ctrl(&ctrl_config);
    if (!ctrl) {
        ESP_LOGE(TAG, "new control interface failed");
        goto fail;
    }
    gpio = audio_codec_new_gpio();
    if (!gpio) {
        ESP_LOGE(TAG, "new GPIO interface failed");
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
    if (!codec) {
        ESP_LOGE(TAG, "new codec interface failed");
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

esp_codec_dev_handle_t
audio_codec_device_open(esp_codec_dev_type_t type) {
    if (!audio_codec_acquire()) {
        return NULL;
    }
    esp_codec_dev_cfg_t config = {.dev_type = type, .codec_if = codec, .data_if = data};
    esp_codec_dev_handle_t device = esp_codec_dev_new(&config);
    if (!device) {
        ESP_LOGE(TAG, "new device failed");
        audio_codec_release();
        return NULL;
    }
    /* Closing one direction must not disable the shared ADC/DAC. */
    if (!audio_codec_check("retain codec", esp_codec_set_disable_when_closed(device, false))) {
        goto fail;
    }
    esp_codec_dev_sample_info_t format = {.sample_rate = AUDIO_OUT_SAMPLE_RATE_HZ,
                                          .bits_per_sample = AUDIO_CODEC_BITS_PER_SAMPLE,
                                          .channel = AUDIO_CODEC_CHANNELS};
    if (!audio_codec_check("open device", esp_codec_dev_open(device, &format))) {
        goto fail;
    }
    device_took_channels = true;
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
    (void)audio_codec_check("close device", esp_codec_dev_close(device));
    esp_codec_dev_delete(device);
    audio_codec_release();
}
