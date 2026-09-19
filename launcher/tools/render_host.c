/*
 * render_host - the one procedure behind every host render. See
 * render_host.h for what a scene declares and which options land here.
 *
 * Not built by idf.py, not part of test/run_tests.sh: standalone binaries,
 * one per scene, built by render_scenes.sh from the real firmware
 * translation units the scene names.
 *
 * No device, no serial, no file the firmware knows about: gfx_init()
 * mallocs a plain framebuffer, the scene draws into it exactly as it would
 * on the real panel, and this reads gfx_framebuffer() straight back out.
 * The BMP encoding is util/screenshot.h's - already pure, already
 * host-portable, already tested (test/suites/suite_screenshot.c).
 */

#include "render_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "gfx/gfx_color.h"
#include "ui/ui_transform.h"
#include "util/screenshot.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#define DEFAULT_DT_MS 16

/* Where the pixel read at (x, y) of the output image lives in the
 * framebuffer. For a panel-native dump that is the same pixel; otherwise
 * the output is the UPRIGHT LOGICAL canvas, and each of its pixels is
 * mapped through the very transform the scene drew with. */
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

typedef struct {
    int width, height;
    long bytes;
} render_size_t;

static bool
write_bmp(FILE* out, int quarter, bool panel, render_size_t* size) {
    const bool upright = quarter % 2 == 0;
    const int out_w = (panel || upright) ? GFX_WIDTH : GFX_HEIGHT;
    const int out_h = (panel || upright) ? GFX_HEIGHT : GFX_WIDTH;
    const ui_transform_t t = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);

    const gfx_color_t* fb = gfx_framebuffer();
    if (fb == NULL) {
        /* Band mode keeps no retained frame to read back, so there is
         * nothing to write - the same refusal a device capture makes. A
         * scene wanting an image asks for the full-framebuffer layout. */
        fprintf(stderr, "no framebuffer to read: the scene left gfx in band mode\n");
        return false;
    }
    const int32_t stride = screenshot_bmp_row_stride(out_w);

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, out_w, out_h);
    fwrite(header, 1, sizeof(header), out);

    uint8_t* row = calloc(1, (size_t)stride);
    if (row == NULL) {
        fprintf(stderr, "out of memory\n");
        return false;
    }

    /* Bottom-up, per screenshot_bmp_header()'s own contract (positive
     * biHeight). */
    for (int y = out_h - 1; y >= 0; y--) {
        for (int x = 0; x < out_w; x++) {
            const uint32_t rgb = gfx_color_rgb888(sample(fb, t, panel, x, y));
            row[x * 3 + 0] = (uint8_t)(rgb);       /* B */
            row[x * 3 + 1] = (uint8_t)(rgb >> 8);  /* G */
            row[x * 3 + 2] = (uint8_t)(rgb >> 16); /* R */
        }
        fwrite(row, 1, (size_t)stride, out);
    }

    free(row);
    size->width = out_w;
    size->height = out_h;
    size->bytes = (long)SCREENSHOT_BMP_HEADER_SIZE + (long)stride * out_h;
    return true;
}

/* The declared steps turned into one frame's input_t. `down` holds from
 * the step that set it, so edges come out of the change rather than out of
 * a second declaration of the same tap. */
static void
apply_input(const render_scene_t* scene, int index, input_t* state) {
    const bool was_down = state->down;

    for (int i = 0; i < scene->input_count; i++) {
        const render_input_step_t* step = &scene->input[i];
        if (step->frame == index) {
            state->down = step->down;
            state->x = step->x;
            state->y = step->y;
        }
    }

    state->pressed = state->down && !was_down;
    state->released = !state->down && was_down;
    if (state->pressed) {
        state->press_x = state->x;
        state->press_y = state->y;
    }
}

static int
usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [--quarter N] [--panel] [--frames N] [--dt N] [-o PATH]\n"
            "       render_host.h lists what each one does; the scene may take more.\n",
            argv0);
    return 2;
}

int
main(int argc, char** argv) {
    const render_scene_t* scene = &render_scene;

    int quarter = scene->quarter;
    int frames = scene->frames > 0 ? scene->frames : 1;
    uint32_t dt_ms = scene->dt_ms > 0 ? scene->dt_ms : DEFAULT_DT_MS;
    bool panel = false;
    const char* out_path = NULL;

    char* rest[64];
    int rest_count = 0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const bool has_value = i + 1 < argc;
        if (strcmp(a, "--panel") == 0) {
            panel = true;
        } else if (strcmp(a, "--quarter") == 0 && has_value) {
            quarter = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--frames") == 0 && has_value) {
            frames = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--dt") == 0 && has_value) {
            dt_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "-o") == 0 && has_value) {
            out_path = argv[++i];
        } else if (rest_count < (int)(sizeof(rest) / sizeof(rest[0]))) {
            rest[rest_count++] = argv[i];
        } else {
            return usage(argv[0]);
        }
    }

    if (quarter < 0 || quarter > 3 || frames < 1) {
        return usage(argv[0]);
    }
    if (scene->options != NULL) {
        if (!scene->options(rest_count, rest)) {
            return usage(argv[0]);
        }
    } else if (rest_count > 0) {
        return usage(argv[0]);
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
    if (scene->setup != NULL && !scene->setup(quarter)) {
        fprintf(stderr, "%s: setup failed\n", scene->name);
        return 1;
    }

    render_frame_t frame = {.count = frames, .quarter = quarter, .dt_ms = dt_ms};
    for (int i = 0; i < frames; i++) {
        frame.index = i;
        frame.elapsed_ms = (uint32_t)i * dt_ms;
        apply_input(scene, i, &frame.input);
        scene->draw(&frame);
    }

    FILE* out = stdout;
    if (out_path != NULL) {
        out = fopen(out_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "%s: cannot write %s\n", scene->name, out_path);
            return 1;
        }
    }

    render_size_t size = {0};
    const bool ok = write_bmp(out, quarter, panel, &size);
    if (out != stdout) {
        fclose(out);
        /* The shape the caller checks its own declaration against. Only for
         * a file: a render to stdout is read by a script that wants the
         * scene's own first stderr line, not this one. */
        fprintf(stderr, "RENDER %s %dx%d %ld\n", scene->name, size.width, size.height, size.bytes);
    }
    return ok ? 0 : 1;
}
