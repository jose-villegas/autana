#include "selftest/post.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "board/board.h"
#include "core/memory.h"
#include "core/timing.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_present.h"
#include "power/power_decode.h"
#include "sound/sound_status.h"

static const char* TAG = "post";

/* Below this the board is starved and later contiguous allocations
 * start failing confusingly, worth catching here where the message is
 * clear. Checked against memory_largest_block(MEMORY_DMA), not total free
 * heap. DELIBERATELY WELL BELOW what an app's largest buffer needs, and
 * this is not the check that guards that: pegging it there would
 * false-alarm a healthy development image only a kilobyte or so above it,
 * and the fit is checked precisely elsewhere. */
#define MIN_LARGEST_DMA_BLOCK (32 * 1024)

#define EXPECTED_FLASH_BYTES  (16 * 1024 * 1024)

/* Results are retained rather than only logged. A board in the field may have
 * nothing attached to its serial port, so they have to be showable on screen. */
static post_result_t results[POST_MAX_CHECKS];
static int checks_run;
static int checks_failed;

static void
report(const char* name, bool ok, post_severity_t severity, const char* detail) {
    const char* mark;
    if (ok) {
        mark = "ok  ";
    } else if (severity == POST_OPTIONAL) {
        mark = "--  "; /* absent, but allowed to be */
    } else {
        mark = "FAIL";
        checks_failed++;
    }

    if (checks_run < POST_MAX_CHECKS) {
        post_result_t* r = &results[checks_run];
        r->name = name;
        r->ok = ok;
        r->severity = severity;
        (void)snprintf(r->detail, sizeof(r->detail), "%s", detail ? detail : "");
    }
    checks_run++;

    ESP_LOGI(TAG, "  [%s] %-16s %s", mark, name, detail ? detail : "");
}

const post_result_t*
post_results(void) {
    return results;
}

int
post_result_count(void) {
    return checks_run < POST_MAX_CHECKS ? checks_run : POST_MAX_CHECKS;
}

int
post_failure_count(void) {
    return checks_failed;
}

void
post_run_before_display(void) {
    checks_run = 0;
    checks_failed = 0;

    ESP_LOGI(TAG, "power-on self test (storage)");

    /* SD is native SDMMC on its own pins, so this is just the natural place
     * to test it first, before gfx_init() takes SPI2 for the display. */
    board_sd_info_t card;
    const esp_err_t err = board_sd_probe(&card);

    if (err == ESP_OK) {
        char detail[96];
        (void)snprintf(detail, sizeof(detail), "%s, %llu MB, mounted and released", card.name,
                       (unsigned long long)(card.bytes >> 20));
        report("sd card", true, POST_OPTIONAL, detail);
    } else {
        /* No card is a normal state, not a fault, hence OPTIONAL. */
        report("sd card", false, POST_OPTIONAL, "no card / not mountable");
    }
}

/* Re-tests the card while the shell is running: SD has its own SDMMC bus,
 * so this is a plain re-mount/unmount, useful for catching a card inserted
 * or removed after POST ran. */
static void
check_sdcard_live(void) {
    const int64_t t0 = timing_now_us();

    board_sd_info_t info;
    char card[40] = "no card";
    const esp_err_t err = board_sd_probe(&info);
    if (err == ESP_OK) {
        (void)snprintf(card, sizeof(card), "%s, %llu MB", info.name, (unsigned long long)(info.bytes >> 20));
    }

    /* SD has its own bus here, so there is nothing to hand back and forth;
     * only the mount/unmount cost is worth timing. */
    const int64_t t_end = timing_now_us();
    ESP_LOGI(TAG, "sd round trip: card %lld us", (long long)(t_end - t0));

    /* Matches post_result_t::detail's size exactly, like every other check in
     * this file; report()'s copy into it can never truncate what fit here. */
    char detail[96];
    (void)snprintf(detail, sizeof(detail), "%s (live, %lld ms round trip)", card, (long long)((t_end - t0) / 1000));

    report("sd card", err == ESP_OK, POST_OPTIONAL, detail);
}

static void
check_soc(void) {
    esp_chip_info_t info;
    esp_chip_info(&info);

    char detail[96];
    (void)snprintf(detail, sizeof(detail), "rev %d, %d core, %s%s%s", info.revision, info.cores,
                   (info.features & CHIP_FEATURE_WIFI_BGN) ? "wifi " : "",
                   (info.features & CHIP_FEATURE_BLE) ? "ble " : "",
                   (info.features & CHIP_FEATURE_IEEE802154) ? "802.15.4" : "");
    report("soc", true, POST_REQUIRED, detail);
}

static void
check_flash(void) {
    uint32_t size = 0;
    const bool ok = esp_flash_get_size(NULL, &size) == ESP_OK;

    char detail[96];
    (void)snprintf(detail, sizeof(detail), "%" PRIu32 " MB%s", size / (1024 * 1024),
                   (ok && size != EXPECTED_FLASH_BYTES) ? " (expected 16)" : "");

    report("flash", ok && size == EXPECTED_FLASH_BYTES, POST_REQUIRED, detail);
}

static void
check_memory(void) {
    /* free and largest must come from the SAME pool or their difference
     * is meaningless: memory_free_bytes(MEMORY_8BIT) sums a second, physically
     * separate ~11 KiB DMA region (the ROM-stack area) that is never
     * contiguous with the main heap, so comparing it against
     * memory_largest_block()'s single-region answer invents a
     * "fragmentation" gap that was never real. Both sides here are
     * MEMORY_DMA. */
    const size_t free_dma = memory_free_bytes(MEMORY_DMA);
    const size_t largest_dma = memory_largest_block(MEMORY_DMA);

    char detail[96];
    (void)snprintf(detail, sizeof(detail), "%u KiB free, DMA block %u KiB", (unsigned)(free_dma / 1024),
                   (unsigned)(largest_dma / 1024));
    /* Gated on the largest block, not total free: "can this board still
     * allocate the things it needs" (an app's largest single buffer
     * is 41,216 bytes and must be contiguous) is a contiguity question, not a total-bytes one. */
    report("memory", largest_dma > MIN_LARGEST_DMA_BLOCK, POST_REQUIRED, detail);

    /* This board always has octal PSRAM; its absence means the code is
     * running on hardware other than what it assumes, which is worth
     * knowing. */
    const size_t psram = memory_psram_bytes();

    char psram_detail[64];
    if (psram != 0) {
        (void)snprintf(psram_detail, sizeof(psram_detail), "%u MiB present", (unsigned)(psram / (1024 * 1024)));
    } else {
        (void)snprintf(psram_detail, sizeof(psram_detail), "absent - unexpected");
    }
    report("psram", psram != 0, POST_REQUIRED, psram_detail);
}

static void
check_mac(void) {
    uint8_t mac[6] = {0};
    const bool ok = esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK;

    char detail[96];
    (void)snprintf(detail, sizeof(detail), "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4],
                   mac[5]);

    /* An all-zero MAC means eFuse was never programmed or cannot be read,
     * which breaks every radio before it starts. */
    const bool valid = ok && (mac[0] | mac[1] | mac[2] | mac[3] | mac[4] | mac[5]);
    report("mac / efuse", valid, POST_REQUIRED, detail);
}

static void
check_temperature(void) {
    float celsius = 0.0F;
    const board_temp_sensor_status_t status = board_temp_sensor_read_celsius(&celsius);

    char detail[96];
    switch (status) {
        case BOARD_TEMP_SENSOR_INSTALL_FAILED: (void)snprintf(detail, sizeof(detail), "install failed"); break;
        case BOARD_TEMP_SENSOR_READ_FAILED: (void)snprintf(detail, sizeof(detail), "read failed"); break;
        case BOARD_TEMP_SENSOR_OK: (void)snprintf(detail, sizeof(detail), "%.1f C", (double)celsius); break;
    }

    /* A plausible reading also rules out a sensor stuck at a fixed value. */
    const bool valid = status == BOARD_TEMP_SENSOR_OK && celsius > -40.0F && celsius < 125.0F;
    report("temp sensor", valid, POST_REQUIRED, detail);
}

static void
check_pmu(void) {
    power_battery_t battery;
    const bool pmu_ok = power_read_battery_now(&battery, NULL);
    char detail[sizeof results[0].detail];
    int length;
    if (!pmu_ok) {
        length = snprintf(detail, sizeof detail, "AXP2101: not responding");
    } else if (!battery.battery_present) {
        length = snprintf(detail, sizeof detail, "AXP2101: battery absent, %s", battery.usb_present ? "USB" : "no USB");
    } else {
        length = snprintf(detail, sizeof detail, "AXP2101: %u mV %u%%, %s, %s (%s)", (unsigned)battery.battery_mv,
                          (unsigned)battery.percent, battery.usb_present ? "USB" : "no USB",
                          power_flow_name(battery.flow), power_charge_name(battery.charge));
    }
    const bool formatted = length >= 0 && (size_t)length < sizeof detail;
    report("pmu", pmu_ok && formatted, POST_REQUIRED, formatted ? detail : "battery format failed");
}

/* Every I2C peripheral shares one bus, so a single probe per address
 * establishes whether each chip is alive and addressable. This is the heart of
 * the POST; it is how "can we talk to the gyro" gets answered. */
static void
check_i2c_devices(void) {
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (bus == NULL) {
        report("i2c bus", false, POST_REQUIRED, "not initialised");
        return;
    }
    char pins[32];
    (void)snprintf(pins, sizeof pins, "SDA %d, SCL %d", BOARD_I2C_SDA_GPIO, BOARD_I2C_SCL_GPIO);
    report("i2c bus", true, POST_REQUIRED, pins);

    static const struct {
        const char* name;
        uint16_t address;
        const char* what;
        post_severity_t severity;
    } devices[] = {
        {"io expander", BOARD_IO_EXPANDER_I2C_ADDR, "TCA9554 reset lines", POST_OPTIONAL},
        {"imu", BOARD_IMU_I2C_ADDR, "QMI8658 accel+gyro", POST_REQUIRED},
        {"rtc", BOARD_RTC_I2C_ADDR, "PCF85063 clock", POST_REQUIRED},
    };

    for (unsigned i = 0; i < sizeof(devices) / sizeof(devices[0]); i++) {
        const bool present = i2c_master_probe(bus, devices[i].address, 100) == ESP_OK;

        char detail[96];
        (void)snprintf(detail, sizeof(detail), "0x%02x  %s", devices[i].address, devices[i].what);
        report(devices[i].name, present, devices[i].severity, detail);
    }

    check_pmu();

    /* Touch sits at a different address per board revision, so report which
     * one answered rather than probing a single expected address. */
    const bool ft = i2c_master_probe(bus, BOARD_TOUCH_FT_I2C_ADDR, 100) == ESP_OK;
    const bool cst = i2c_master_probe(bus, BOARD_TOUCH_CST_I2C_ADDR, 100) == ESP_OK;

    char touch_detail[40];
    if (ft) {
        (void)snprintf(touch_detail, sizeof(touch_detail), "0x%02x  %s (V1)", BOARD_TOUCH_FT_I2C_ADDR,
                       BOARD_TOUCH_FT_NAME);
    } else if (cst) {
        (void)snprintf(touch_detail, sizeof(touch_detail), "0x%02x  CST820 (V2)", BOARD_TOUCH_CST_I2C_ADDR);
    } else {
        (void)snprintf(touch_detail, sizeof(touch_detail), "no controller answered");
    }
    report("touch", ft || cst, POST_REQUIRED, touch_detail);
}

static void
check_audio_codec(void) {
    const char* step = sound_init_failure();
    char detail[96];
    int length;
    if (step) {
        length = snprintf(detail, sizeof detail, "audio: init failed (%s)", step);
    } else {
        length = snprintf(detail, sizeof detail, "0x%02x  ES8311", BOARD_AUDIO_CODEC_I2C_ADDR);
    }
    report("audio codec", step == NULL && length >= 0, POST_REQUIRED, length >= 0 ? detail : step);
}

static void
check_display(void) {
    char detail[96];
    (void)snprintf(detail, sizeof(detail), "%dx%d %s", GFX_WIDTH, GFX_HEIGHT, board_variant_name(board_variant()));
    report("display", gfx_framebuffer() != NULL, POST_REQUIRED, detail);
}

void
post_rerun(void) {
    checks_run = 0;
    checks_failed = 0;

    ESP_LOGI(TAG, "re-running self test");

    /* SD has its own SDMMC bus, so this never touches the panel; it goes on
     * showing its last frame throughout, because it refreshes from its own
     * GRAM without the MCU. */
    check_sdcard_live();

    post_run_after_display();
}

bool
post_run_after_display(void) {
    check_soc();
    check_flash();
    check_memory();
    check_mac();
    check_temperature();
    check_i2c_devices();
    check_audio_codec();
    check_display();

    if (checks_failed == 0) {
        ESP_LOGI(TAG, "%d checks, all passed", checks_run);
    } else {
        ESP_LOGE(TAG, "%d checks, %d FAILED", checks_run, checks_failed);
    }

    /* A machine-readable line, so a production rig can grep one string rather
     * than parse the table above. */
    printf("POST_COMPLETE checks=%d failures=%d\n", checks_run, checks_failed);
    (void)fflush(stdout);

    return checks_failed == 0;
}
