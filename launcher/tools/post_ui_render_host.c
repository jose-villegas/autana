/*
 * post_ui_render_host - render the power-on self-test screen with the REAL
 * firmware code (post_ui.c + post_layout.c + gfx.c, unmodified) on a host
 * build, and write it out as a BMP, so the layout can be judged without a
 * flash cycle.
 *
 *     post_ui_render_host <quarter> [--failures] [--panel]  > frame.bmp
 *
 * `quarter` is numbered as display.h numbers it. The image comes out the way
 * the board is READ at that quarter - 448x368 for a landscape one - because
 * that is what gets looked at; --panel writes the framebuffer the way the
 * panel holds it instead, 368x448, which is the shape a device screenshot
 * can be compared against. The rotation is done with the same transform the
 * drawing went through, not a second derivation of it.
 *
 * Not built by idf.py, not part of test/run_tests.sh - a standalone binary,
 * built by post_ui_render_host.sh beside it, the same way
 * boot_anim_render_host.c is.
 *
 * post.c is deliberately NOT linked: its checks probe real I2C, flash and
 * eFuse. The three functions it would have supplied are defined at the
 * bottom of this file over a FIXTURE table - invented sample data, so
 * nothing this tool draws is a reading from any board.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/post.h"
#include "boot/post_layout.h"
#include "boot/post_ui.h"
#include "gfx/gfx.h"
#include "gfx/gfx_color.h"
#include "ui/ui_transform.h"
#include "util/screenshot.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

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

/* Where the pixel read at (x, y) of the output image lives in the
 * framebuffer. For a panel-native dump that is the same pixel; otherwise the
 * output is the UPRIGHT LOGICAL canvas, and each of its pixels is mapped
 * through the very transform post_ui.c drew with. */
static gfx_color_t
sample(const gfx_color_t* fb, ui_transform_t t, bool panel, int x, int y) {
    int px = x;
    int py = y;
    if (!panel) {
        const mu_Rect mapped = ui_transform_rect(t, (mu_Rect){x, y, 1, 1});
        px = mapped.x;
        py = mapped.y;
    }
    if (px < 0 || px >= GFX_WIDTH || py < 0 || py >= GFX_HEIGHT) {
        return gfx_rgb(0x000000);
    }
    return fb[(size_t)py * GFX_WIDTH + px];
}

int
main(int argc, char** argv) {
    int quarter = -1;
    bool failures_only = false;
    bool panel = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--failures") == 0) {
            failures_only = true;
        } else if (strcmp(argv[i], "--panel") == 0) {
            panel = true;
        } else if (quarter < 0) {
            quarter = (int)strtol(argv[i], NULL, 10);
        } else {
            quarter = -1;
            break;
        }
    }
    if (quarter < 0 || quarter > 3) {
        fprintf(stderr, "usage: %s <quarter 0-3> [--failures] [--panel]  > frame.bmp\n", argv[0]);
        return 1;
    }

#if defined(_WIN32)
    /* stdout is text mode by default on Windows, which would rewrite every
     * 0x0A pixel byte into a 0x0D 0x0A pair - silently corrupting the image
     * rather than failing loudly. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (!gfx_init()) {
        fprintf(stderr, "gfx_init failed\n");
        return 1;
    }

    if (failures_only) {
        fail_two_checks();
    }

    const post_ui_report_t report = {
        .title = failures_only ? POST_LAYOUT_FAULT_TITLE : POST_LAYOUT_TITLE,
        .footer = failures_only ? POST_LAYOUT_FAULT_FOOTER : NULL,
        .quarter = quarter,
        .failures_only = failures_only,
    };
    post_ui_draw_report(&report);

    const bool upright = quarter % 2 == 0;
    const int out_w = (panel || upright) ? GFX_WIDTH : GFX_HEIGHT;
    const int out_h = (panel || upright) ? GFX_HEIGHT : GFX_WIDTH;
    const ui_transform_t t = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);

    const gfx_color_t* fb = gfx_framebuffer();
    const int32_t stride = screenshot_bmp_row_stride(out_w);

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, out_w, out_h);
    fwrite(header, 1, sizeof(header), stdout);

    uint8_t* row = calloc(1, (size_t)stride);
    if (row == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    /* Bottom-up, per screenshot_bmp_header()'s own contract. */
    for (int y = out_h - 1; y >= 0; y--) {
        for (int x = 0; x < out_w; x++) {
            const uint32_t rgb = gfx_color_rgb888(sample(fb, t, panel, x, y));
            row[x * 3 + 0] = (uint8_t)(rgb);       /* B */
            row[x * 3 + 1] = (uint8_t)(rgb >> 8);  /* G */
            row[x * 3 + 2] = (uint8_t)(rgb >> 16); /* R */
        }
        fwrite(row, 1, (size_t)stride, stdout);
    }

    free(row);
    return 0;
}
