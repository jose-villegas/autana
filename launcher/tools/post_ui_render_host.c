/*
 * post_ui_render_host - the power-on self-test screen, drawn by the REAL
 * firmware code (post_ui.c + post_layout.c + gfx.c, unmodified) on a host
 * build, so the layout can be judged without a flash cycle.
 *
 *     post_ui_render_host [--quarter N] [--panel] [--failures]  > frame.bmp
 *
 * A render_host.h scene; render_host.c owns main(), the quarter turn and
 * the BMP. Built by render_scenes.sh from post_ui.scene beside it.
 *
 * post.c is deliberately NOT linked: its checks probe real I2C, flash and
 * eFuse. The three functions it would have supplied are defined below over
 * a FIXTURE table - invented sample data, so nothing this tool draws is a
 * reading from any board.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "boot/post.h"
#include "boot/post_layout.h"
#include "boot/post_ui.h"
#include "render_host.h"

/* FIXTURE, not a measurement: sample strings shaped like the ones post.c's
 * format strings produce, so the rendered screen has realistic line lengths
 * to lay out. Nothing here was read from hardware. */
static post_result_t fixture[] = {
    {"sd card", true, POST_OPTIONAL, "SDABC, 29820 MB (live, 51 ms round trip)"},
    {"soc", true, POST_REQUIRED, "rev 2, 2 core, wifi ble "},
    {"flash", true, POST_REQUIRED, "16 MB"},
    {"memory", true, POST_REQUIRED, "140 KiB free, DMA block 76 KiB"},
    {"psram", true, POST_REQUIRED, "8 MiB present"},
    {"mac / efuse", true, POST_REQUIRED, "90:70:69:fe:a3:08"},
    {"temp sensor", true, POST_REQUIRED, "38.5 C"},
    {"i2c bus", true, POST_REQUIRED, "port 0, SDA 15, SCL 14"},
    {"io expander", true, POST_OPTIONAL, "0x20  TCA9554 reset lines"},
    {"pmu", true, POST_REQUIRED, "0x34  AXP2101 power"},
    {"imu", true, POST_REQUIRED, "0x6b  QMI8658 accel+gyro"},
    {"rtc", true, POST_REQUIRED, "0x51  PCF85063 clock"},
    {"touch", true, POST_REQUIRED, "0x15  CST820 (V2)"},
    {"audio codec", true, POST_REQUIRED, "0x18  ES8311"},
    {"display", true, POST_REQUIRED, "368x448 CO5300 + CST820 (V2)"},
};

static bool failures_only;

static int
fixture_count(void) {
    return (int)(sizeof(fixture) / sizeof(fixture[0]));
}

/* Fails two required checks, so the fault screen has something to show. */
static void
fail_two_checks(void) {
    for (int i = 0; i < fixture_count(); i++) {
        if (strcmp(fixture[i].name, "psram") == 0) {
            fixture[i].ok = false;
            snprintf(fixture[i].detail, sizeof(fixture[i].detail), "absent - unexpected");
        } else if (strcmp(fixture[i].name, "touch") == 0) {
            fixture[i].ok = false;
            snprintf(fixture[i].detail, sizeof(fixture[i].detail), "no controller answered");
        }
    }
}

const post_result_t*
post_results(void) {
    return fixture;
}

int
post_result_count(void) {
    return fixture_count();
}

int
post_failure_count(void) {
    int failed = 0;
    for (int i = 0; i < fixture_count(); i++) {
        if (!fixture[i].ok && fixture[i].severity == POST_REQUIRED) {
            failed++;
        }
    }
    return failed;
}

static bool
options(int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--failures") == 0) {
            failures_only = true;
        } else {
            return false;
        }
    }
    return true;
}

static bool
setup(int quarter) {
    (void)quarter;
    if (failures_only) {
        fail_two_checks();
    }
    return true;
}

static void
draw(const render_frame_t* frame) {
    const post_ui_report_t report = {
        .title = failures_only ? POST_LAYOUT_FAULT_TITLE : POST_LAYOUT_TITLE,
        .footer = failures_only ? POST_LAYOUT_FAULT_FOOTER : NULL,
        .quarter = frame->quarter,
        .failures_only = failures_only,
    };
    post_ui_draw_report(&report);
}

const render_scene_t render_scene = {
    .name = "post_ui",
    .quarter = 1,
    .frames = 1,
    .options = options,
    .setup = setup,
    .draw = draw,
};
