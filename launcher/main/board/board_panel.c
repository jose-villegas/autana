/*
 * board_panel: ESP32-S3 binding of the panel link (Waveshare
 * ESP32-S3-Touch-AMOLED-1.8): the QSPI pins and both revisions' init
 * sequences.
 */
#include "board/board_panel.h"
#include "board/board.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_sh8601.h"
#include "esp_log.h"

static const char* TAG = "board_panel";

/* Copied from the Waveshare BSP (Apache-2.0, (c) 2026 Waveshare Team),
 * where it is a private static; needed here because the panel is brought
 * up here rather than calling bsp_display_new(), which offers no
 * way to reach the init sequence at all. Command 0x11 (sleep out) carries
 * a 120 ms settle, dominating a full init's cost. */
static const sh8601_lcd_init_cmd_t lcd_init_cmds[] = {
    {0x11, (uint8_t[]){0x00}, 0, 120},
    {0x44, (uint8_t[]){0x01, 0xD1}, 2, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 10},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x51, (uint8_t[]){0x00}, 1, 10},
    {0x29, (uint8_t[]){0x00}, 0, 10},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
};

/* The V2 revision (CO5300 panel). From Waveshare's own esp-idf colour-bar
 * example for this board. */
static const co5300_lcd_init_cmd_t co5300_init_cmds[] = {
    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x00, 0x01, 0x6F}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xBF}, 4, 0},
    {0x11, NULL, 0, 100},
    {0x29, NULL, 0, 0},
};

/* Common to both panel drivers: claims SPI2 for the QSPI lines, with the
 * same pad-strength opt-in either way. */
static esp_err_t
qspi_bus_up(size_t max_transfer_bytes) {
    const spi_bus_config_t bus = {
        .sclk_io_num = BSP_LCD_PCLK,
        .data0_io_num = BSP_LCD_DATA0,
        .data1_io_num = BSP_LCD_DATA1,
        .data2_io_num = BSP_LCD_DATA2,
        .data3_io_num = BSP_LCD_DATA3,
        .max_transfer_sz = max_transfer_bytes,
    };

    esp_err_t err = spi_bus_initialize(BSP_LCD_SPI_NUM, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

#if defined(CONFIG_LAUNCHER_GFX_QSPI_STRONG_PADS) && CONFIG_LAUNCHER_GFX_QSPI_STRONG_PADS
    /* AFTER spi_bus_initialize(), which is what configures these pads; set
     * before it and the driver overwrites the setting. Does not bring 80 MHz
     * back inside the panel's rating; see the option's help text. */
    {
        static const gpio_num_t qspi_pads[] = {
            BSP_LCD_PCLK, BSP_LCD_DATA0, BSP_LCD_DATA1, BSP_LCD_DATA2, BSP_LCD_DATA3,
        };
        for (size_t i = 0; i < sizeof(qspi_pads) / sizeof(qspi_pads[0]); i++) {
            const esp_err_t derr = gpio_set_drive_capability(qspi_pads[i], GPIO_DRIVE_CAP_3);
            if (derr != ESP_OK) {
                ESP_LOGW(TAG, "drive capability on pad %d: %s", (int)qspi_pads[i], esp_err_to_name(derr));
            }
        }
    }
#endif
    return ESP_OK;
}

/* The panel io and driver objects at `hz`. Creating them sends nothing to
 * the panel, which is what lets a clock change reopen them without
 * re-running bring-up. */
static esp_err_t
panel_open_sh8601(int hz, esp_lcd_panel_io_color_trans_done_cb_t on_sent, esp_lcd_panel_io_handle_t* io,
                  esp_lcd_panel_handle_t* panel) {
    esp_lcd_panel_io_spi_config_t io_config = SH8601_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, on_sent, NULL);
    io_config.pclk_hz = hz;
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, io);
    if (err != ESP_OK) {
        return err;
    }

    sh8601_vendor_config_t vendor = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = sizeof(lcd_init_cmds) / sizeof(lcd_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC, /* no dedicated reset line; see board_detect() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    return esp_lcd_new_panel_sh8601(*io, &panel_config, panel);
}

static esp_err_t
panel_open_co5300(int hz, esp_lcd_panel_io_color_trans_done_cb_t on_sent, esp_lcd_panel_io_handle_t* io,
                  esp_lcd_panel_handle_t* panel) {
    esp_lcd_panel_io_spi_config_t io_config = CO5300_PANEL_IO_QSPI_CONFIG(BSP_LCD_CS, on_sent, NULL);
    io_config.pclk_hz = hz;
    esp_err_t err = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)BSP_LCD_SPI_NUM, &io_config, io);
    if (err != ESP_OK) {
        return err;
    }

    co5300_vendor_config_t vendor = {
        .init_cmds = co5300_init_cmds,
        .init_cmds_size = sizeof(co5300_init_cmds) / sizeof(co5300_init_cmds[0]),
        .flags = {.use_qspi_interface = 1},
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = GPIO_NUM_NC, /* no dedicated reset line; see board_detect() */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
        .vendor_config = &vendor,
    };
    err = esp_lcd_new_panel_co5300(*io, &panel_config, panel);
    if (err != ESP_OK) {
        return err;
    }
    return esp_lcd_panel_set_gap(*panel, BOARD_PANEL_X_GAP, 0);
}

esp_err_t
board_panel_open(int hz, esp_lcd_panel_io_color_trans_done_cb_t on_sent, esp_lcd_panel_io_handle_t* io,
                 esp_lcd_panel_handle_t* panel) {
    ESP_LOGI(TAG, "panel QSPI at %d MHz", hz / 1000000);
    if (board_variant() == BOARD_VARIANT_CO5300_CST) {
        return panel_open_co5300(hz, on_sent, io, panel);
    }
    return panel_open_sh8601(hz, on_sent, io, panel);
}

/* The same steps for either revision; board_panel_open() picks the driver
 * the detected one needs (board_variant_t). */
esp_err_t
board_panel_bring_up(int hz, size_t max_transfer_bytes, esp_lcd_panel_io_color_trans_done_cb_t on_sent,
                     esp_lcd_panel_io_handle_t* io, esp_lcd_panel_handle_t* panel) {
    esp_err_t err = qspi_bus_up(max_transfer_bytes);
    if (err != ESP_OK) {
        return err;
    }
    err = board_panel_open(hz, on_sent, io, panel);
    if (err != ESP_OK) {
        return err;
    }

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(*panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(*panel), TAG, "init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(*panel, true), TAG, "on");
    return ESP_OK;
}
