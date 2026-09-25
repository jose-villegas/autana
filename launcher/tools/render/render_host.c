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
 * host-portable, already tested (test/suites/suite_screenshot.c). --video
 * appends every drawn frame through render_video.c instead of keeping only
 * the last.
 */

#include "render_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "gfx/gfx_color.h"
#include "render_video.h"
#include "ui/ui_transform.h"
#include "util/screenshot.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#define DEFAULT_DT_MS   16

/* RIFF AVI 1.0 keeps its whole size in a 32-bit field; players choke well
 * before that wraps, so a run past this is refused instead of written. */
#define VIDEO_MAX_BYTES ((int64_t)1024 * 1024 * 1024)

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

/* The output canvas for `quarter`/`panel`, decided before gfx_init(): both
 * GFX_WIDTH and GFX_HEIGHT are compile-time, so a --video run can be sized
 * and refused, if it must be, before anything is drawn. */
static void
output_size(int quarter, bool panel, int* out_w, int* out_h) {
    const bool upright = quarter % 2 == 0;
    *out_w = (panel || upright) ? GFX_WIDTH : GFX_HEIGHT;
    *out_h = (panel || upright) ? GFX_HEIGHT : GFX_WIDTH;
}

/* Fills `buf` (screenshot_bmp_row_stride(out_w) * out_h bytes) with one
 * frame's bottom-up 24-bit BGR pixels - the body a BMP and an AVI 'DIB '
 * chunk both carry unchanged, so the BMP write and every --video frame
 * share this instead of each walking the framebuffer on its own. */
static void
convert_frame(const gfx_color_t* fb, ui_transform_t t, bool panel, int out_w, int out_h, int32_t stride, uint8_t* buf) {
    for (int y = out_h - 1, row = 0; y >= 0; y--, row++) {
        uint8_t* dst = buf + (size_t)row * stride;
        for (int x = 0; x < out_w; x++) {
            const uint32_t rgb = gfx_color_rgb888(sample(fb, t, panel, x, y));
            dst[x * 3 + 0] = (uint8_t)(rgb);       /* B */
            dst[x * 3 + 1] = (uint8_t)(rgb >> 8);  /* G */
            dst[x * 3 + 2] = (uint8_t)(rgb >> 16); /* R */
        }
    }
}

typedef struct {
    int width, height;
    long bytes;
} render_size_t;

static bool
write_bmp(FILE* out, const uint8_t* frame_buf, int out_w, int out_h, render_size_t* size) {
    const int32_t stride = screenshot_bmp_row_stride(out_w);

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, out_w, out_h);
    fwrite(header, 1, sizeof(header), out);
    fwrite(frame_buf, 1, (size_t)stride * out_h, out);

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
            "usage: %s [--quarter N] [--panel] [--frames N] [--dt N] [-o PATH] [--video PATH]\n"
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
    const char* video_path = NULL;

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
        } else if (strcmp(a, "--video") == 0 && has_value) {
            video_path = argv[++i];
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

    int out_w, out_h;
    output_size(quarter, panel, &out_w, &out_h);

    if (video_path != NULL) {
        if (dt_ms == 0) {
            fprintf(stderr, "%s: --video needs a nonzero --dt\n", scene->name);
            return 1;
        }
        const int64_t total = render_video_total_bytes(out_w, out_h, frames);
        if (total > VIDEO_MAX_BYTES) {
            const int64_t fits = render_video_frames_that_fit(out_w, out_h, VIDEO_MAX_BYTES);
            fprintf(stderr,
                    "%s: %d frames at %dx%d would write %lld MB, over RIFF AVI 1.0's 1 GB limit; "
                    "at this size and --dt %u, at most %lld frames fit\n",
                    scene->name, frames, out_w, out_h, (long long)(total / (1024 * 1024)), dt_ms, (long long)fits);
            return 1;
        }
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

    const gfx_color_t* fb = gfx_framebuffer();
    if (fb == NULL) {
        /* Band mode keeps no retained frame to read back, so there is
         * nothing to write - the same refusal a device capture makes. A
         * scene wanting an image asks for the full-framebuffer layout. */
        fprintf(stderr, "no framebuffer to read: the scene left gfx in band mode\n");
        return 1;
    }
    const ui_transform_t t = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
    const int32_t stride = screenshot_bmp_row_stride(out_w);

    uint8_t* frame_buf = malloc((size_t)stride * out_h);
    if (frame_buf == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    render_video_t video;
    if (video_path != NULL && !render_video_open(&video, video_path, out_w, out_h, dt_ms)) {
        fprintf(stderr, "%s: cannot write %s\n", scene->name, video_path);
        free(frame_buf);
        return 1;
    }

    render_frame_t frame = {.count = frames, .quarter = quarter, .dt_ms = dt_ms};
    bool video_ok = true;
    for (int i = 0; i < frames; i++) {
        frame.index = i;
        frame.elapsed_ms = (uint32_t)i * dt_ms;
        apply_input(scene, i, &frame.input);
        scene->draw(&frame);

        convert_frame(fb, t, panel, out_w, out_h, stride, frame_buf);
        if (video_path != NULL && video_ok) {
            video_ok = render_video_write_frame(&video, frame_buf, stride * out_h);
        }
    }

    if (video_path != NULL) {
        video_ok = render_video_close(&video) && video_ok;
        if (!video_ok) {
            fprintf(stderr, "%s: writing %s failed\n", scene->name, video_path);
            free(frame_buf);
            return 1;
        }
    }

    FILE* out = stdout;
    if (out_path != NULL) {
        out = fopen(out_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "%s: cannot write %s\n", scene->name, out_path);
            free(frame_buf);
            return 1;
        }
    }

    render_size_t size = {0};
    const bool ok = write_bmp(out, frame_buf, out_w, out_h, &size);
    free(frame_buf);
    if (out != stdout) {
        fclose(out);
        /* The shape the caller checks its own declaration against. Only for
         * a file: a render to stdout is read by a script that wants the
         * scene's own first stderr line, not this one. */
        fprintf(stderr, "RENDER %s %dx%d %ld\n", scene->name, size.width, size.height, size.bytes);
    }
    return ok ? 0 : 1;
}
