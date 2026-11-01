/*
 * render_host: the shared host render procedure. Real scene code draws into
 * gfx-owned pictures; gfx_read_panel_row() supplies completed output.
 * BMP and AVI encoding use gfx/capture/screenshot.h. No device access.
 */

#include "render_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console/console_tune.h"
#include "core/tune.h"
#include "draw/draw.h"
#include "gfx/capture/screenshot.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "gfx/present/gfx_present.h"
#include "gfx/target/gfx_color.h"
#include "render_video.h"
#include "render_watch.h"
#include "ui/layout/ui_transform.h"

#include "render/context/render_context.h"
#include "scene/scene_shell.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include "image/pixel/rgb565.h"
#endif

#define DEFAULT_DT_MS   16

/* RIFF AVI 1.0 keeps its whole size in a 32-bit field; players choke well
 * before that wraps, so a run past this is refused instead of written. */
#define VIDEO_MAX_BYTES ((int64_t)1024 * 1024 * 1024)

static void
tune_reply(const char* line) {
    fprintf(stderr, "%s\n", line);
}

bool
render_host_set_tunable(const char* name, int32_t value) {
    if (!tune_set(tune_shared(), name, value)) {
        fprintf(stderr, "render_host: unknown tunable or value outside range: %s\n", name);
        return false;
    }
    return true;
}

bool
render_host_set_tunable_text(const char* assignment) {
    char name[TUNE_NAME_MAX + 1];
    const char* equal = strchr(assignment, '=');
    if (equal == NULL || equal == assignment || equal - assignment > TUNE_NAME_MAX) {
        fprintf(stderr, "render_host: --set needs owner.name=value\n");
        return false;
    }
    const size_t length = (size_t)(equal - assignment);
    memcpy(name, assignment, length);
    name[length] = '\0';
    int32_t value;
    if (!tune_parse_value(equal + 1, &value)) {
        fprintf(stderr, "render_host: invalid tunable value %s\n", equal + 1);
        return false;
    }
    return render_host_set_tunable(name, value);
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
 * frame's bottom-up 24-bit BGR pixels, the body a BMP and an AVI 'DIB '
 * chunk both carry unchanged, so the BMP write and every --video frame
 * share this instead of each walking the framebuffer on its own. */
static void
convert_frame(affine2x_t t, bool panel, int out_w, int out_h, int32_t stride, uint8_t* buf) {
    affine2x_t inverse = affine2x_identity();
    if (!panel) {
        (void)affine2x_invert(t, &inverse);
    }
    gfx_color_t pixels[GFX_WIDTH];
    for (int py = 0; py < GFX_HEIGHT; py++) {
        gfx_read_panel_row(py, pixels);
        for (int px = 0; px < GFX_WIDTH; px++) {
            const recti_t mapped = ui_transform_rect(inverse, (recti_t){px, py, 1, 1});
            if (mapped.x < 0 || mapped.x >= out_w || mapped.y < 0 || mapped.y >= out_h) {
                continue;
            }
            uint8_t* dst = buf + (size_t)(out_h - 1 - mapped.y) * stride + mapped.x * 3;
            const uint32_t rgb = rgb565be_to_rgb888(pixels[px]);
            dst[0] = (uint8_t)rgb;
            dst[1] = (uint8_t)(rgb >> 8);
            dst[2] = (uint8_t)(rgb >> 16);
        }
    }
}

/* The pixel at (x, y) of a converted frame, top-down. */
static const uint8_t*
frame_pixel(const uint8_t* buf, int32_t stride, int out_h, int x, int y) {
    return buf + (size_t)(out_h - 1 - y) * stride + x * 3;
}

/* The corner colour most of the frame's border shares: the background a
 * scene cleared to, even when something is drawn into one corner. */
static const uint8_t*
border_colour(const uint8_t* buf, int32_t stride, int out_w, int out_h) {
    const uint8_t* corners[] = {frame_pixel(buf, stride, out_h, 0, 0), frame_pixel(buf, stride, out_h, out_w - 1, 0),
                                frame_pixel(buf, stride, out_h, 0, out_h - 1),
                                frame_pixel(buf, stride, out_h, out_w - 1, out_h - 1)};
    const uint8_t* best = corners[0];
    int best_count = -1;
    for (size_t c = 0; c < sizeof corners / sizeof corners[0]; c++) {
        int count = 0;
        for (int x = 0; x < out_w; x++) {
            count += memcmp(frame_pixel(buf, stride, out_h, x, 0), corners[c], 3) == 0;
            count += memcmp(frame_pixel(buf, stride, out_h, x, out_h - 1), corners[c], 3) == 0;
        }
        for (int y = 0; y < out_h; y++) {
            count += memcmp(frame_pixel(buf, stride, out_h, 0, y), corners[c], 3) == 0;
            count += memcmp(frame_pixel(buf, stride, out_h, out_w - 1, y), corners[c], 3) == 0;
        }
        if (count > best_count) {
            best = corners[c];
            best_count = count;
        }
    }
    return best;
}

/* The box round every pixel of a converted frame unlike its border's
 * colour; empty for a blank frame. */
static recti_t
drawn_box(const uint8_t* buf, int32_t stride, int out_w, int out_h) {
    const uint8_t* background = border_colour(buf, stride, out_w, out_h);
    recti_t box = {0, 0, 0, 0};
    for (int y = 0; y < out_h; y++) {
        for (int x = 0; x < out_w; x++) {
            if (memcmp(frame_pixel(buf, stride, out_h, x, y), background, 3) != 0) {
                box = recti_union(box, (recti_t){x, y, 1, 1});
            }
        }
    }
    return box;
}

typedef struct {
    int width, height;
    long bytes;
} render_size_t;

/* The part `box` of a converted frame `out_w` x `out_h`, as a BMP. */
static bool
write_bmp(FILE* out, const uint8_t* frame_buf, int out_w, int out_h, recti_t box, render_size_t* size) {
    const int32_t stride = screenshot_bmp_row_stride(out_w);
    const int32_t box_stride = screenshot_bmp_row_stride(box.w);
    static const uint8_t PAD[3];

    uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
    screenshot_bmp_header(header, box.w, box.h);
    fwrite(header, 1, sizeof(header), out);
    for (int y = box.y + box.h - 1; y >= box.y; y--) {
        fwrite(frame_pixel(frame_buf, stride, out_h, box.x, y), 1, (size_t)box.w * 3, out);
        fwrite(PAD, 1, (size_t)(box_stride - box.w * 3), out);
    }

    size->width = box.w;
    size->height = box.h;
    size->bytes = (long)SCREENSHOT_BMP_HEADER_SIZE + (long)box_stride * box.h;
    return true;
}

/* `box` grown by `margin` on every side, kept inside the frame; the whole
 * frame for an empty `box`. */
static recti_t
crop_box(recti_t box, int margin, int out_w, int out_h) {
    const recti_t frame = {0, 0, out_w, out_h};
    if (recti_is_empty(box)) {
        return frame;
    }
    const recti_t grown =
        recti_from_edges(box.x - margin, box.y - margin, recti_right(box) + margin, recti_bottom(box) + margin);
    return recti_intersect(grown, frame);
}

/* The scene manager's compose where a provider did not link it: nothing to
 * compose. scene/scene_shell.c's own definition replaces this. */
__attribute__((weak)) void
scene_shell_compose(uint32_t dt_ms) {
    (void)dt_ms;
}

/* The declared steps turned into one frame's input_t. `down` holds from
 * the step that set it, so edges come out of the change rather than out of
 * a second declaration of the same tap. */
static void
apply_input(const frame_provider_t* provider, int index, input_t* state) {
    const bool was_down = state->down;

    for (int i = 0; i < provider->input_count; i++) {
        const render_input_step_t* step = &provider->input[i];
        if (step->frame == index) {
            state->down = step->down;
            state->point = step->point;
        }
    }

    state->pressed = state->down && !was_down;
    state->released = !state->down && was_down;
    if (state->pressed) {
        state->press = state->point;
    }
}

static int
usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [--quarter N] [--panel] [--frames N] [--dt N] [-o PATH] [--video PATH]\n"
            "       [--set owner.name=value ...] [--tunables] [--crop]\n"
            "       docs/tools/Render-Harness.md lists options; the scene may take more.\n",
            argv0);
    return 2;
}

static bool
refuse(int* exit_code, const char* argv0) {
    *exit_code = usage(argv0);
    return false;
}

/* One open run owns the frame history, readback buffers and output streams. */
static struct {
    const frame_provider_t* provider;
    bool panel, crop, video_ok;
    const char *out_path, *video_path;
    int out_w, out_h;
    affine2x_t t;
    int32_t stride;
    uint8_t* frame_buf;
    render_video_t video;
    char* console_path;
    provider_frame_t frame;
    recti_t drawn;
    affine2x_t canvas_from_panel;
} host;

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
__attribute__((weak)) void
scene_shell_checkpoint_capture(checkpoint_t* saved) {
    (void)saved;
}

__attribute__((weak)) bool
scene_shell_checkpoint_prepare(checkpoint_t* saved) {
    (void)saved;
    return true;
}

struct render_host_checkpoint {
    checkpoint_t state;
};

const uint8_t*
render_host_pixels(size_t* bytes) {
    *bytes = (size_t)host.stride * host.out_h;
    return host.frame_buf;
}

render_host_checkpoint_t*
render_host_save(void) {
    render_host_checkpoint_t* saved = calloc(1, sizeof(*saved));
    if (saved == NULL) {
        return NULL;
    }
    checkpoint_capture(&saved->state, &host.frame, sizeof host.frame);
    checkpoint_capture(&saved->state, &host.drawn, sizeof host.drawn);
    checkpoint_capture_at(&saved->state, &host.frame_buf, (size_t)host.stride * host.out_h);
    scene_shell_checkpoint_capture(&saved->state);
    gfx_checkpoint_capture(&saved->state);
    tune_checkpoint_capture(&saved->state, tune_shared());
    if (saved->state.failed) {
        render_host_checkpoint_free(saved);
        return NULL;
    }
    return saved;
}

bool
render_host_restore(render_host_checkpoint_t* saved) {
    gfx_present_drain();
    return checkpoint_validate(&saved->state) && scene_shell_checkpoint_prepare(&saved->state)
           && checkpoint_restore(&saved->state);
}

void
render_host_checkpoint_free(render_host_checkpoint_t* saved) {
    if (saved != NULL) {
        checkpoint_clear(&saved->state);
        free(saved);
    }
}
#endif

bool
render_host_open(const frame_provider_t* provider, int argc, char** argv, int* exit_code) {
    memset(&host, 0, sizeof host);
    *exit_code = 1;
    host.provider = provider;
    host.frame.quarter = host.provider->quarter;
    host.frame.count = host.provider->frames > 0 ? host.provider->frames : 1;
    host.frame.dt_ms = host.provider->dt_ms > 0 ? host.provider->dt_ms : DEFAULT_DT_MS;
    char* rest[64];
    int rest_count = 0;
    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const bool has_value = i + 1 < argc;
        if (strcmp(a, "--panel") == 0) {
            host.panel = true;
        } else if (strcmp(a, "--quarter") == 0 && has_value) {
            host.frame.quarter = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--tunables") == 0) {
            (void)console_tune_line(tune_shared(), "TUNE", tune_reply);
            *exit_code = 0;
            return false;
        } else if (strcmp(a, "--set") == 0 && has_value) {
            if (!render_host_set_tunable_text(argv[++i])) {
                return refuse(exit_code, argv[0]);
            }
        } else if (strcmp(a, "--frames") == 0 && has_value) {
            host.frame.count = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--dt") == 0 && has_value) {
            host.frame.dt_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "-o") == 0 && has_value) {
            host.out_path = argv[++i];
        } else if (strcmp(a, "--video") == 0 && has_value) {
            host.video_path = argv[++i];
        } else if (strcmp(a, "--crop") == 0) {
            host.crop = true;
        } else if (rest_count < (int)(sizeof(rest) / sizeof(rest[0]))) {
            rest[rest_count++] = argv[i];
        } else {
            return refuse(exit_code, argv[0]);
        }
    }

    if (host.frame.quarter < 0 || host.frame.quarter > 3 || host.frame.count < 1) {
        return refuse(exit_code, argv[0]);
    }
    if (host.provider->options != NULL ? !host.provider->options(rest_count, rest) : rest_count > 0) {
        return refuse(exit_code, argv[0]);
    }

    output_size(host.frame.quarter, host.panel, &host.out_w, &host.out_h);

    if (host.video_path != NULL) {
        if (host.frame.dt_ms == 0) {
            fprintf(stderr, "%s: --video needs a nonzero --dt\n", host.provider->name);
            return false;
        }
        const int64_t total = render_video_total_bytes(host.out_w, host.out_h, host.frame.count);
        if (total > VIDEO_MAX_BYTES) {
            const int64_t fits = render_video_frames_that_fit(host.out_w, host.out_h, VIDEO_MAX_BYTES);
            fprintf(stderr,
                    "%s: %d frames at %dx%d would write %lld MB, over RIFF AVI 1.0's 1 GB limit; "
                    "at this size and --dt %u, at most %lld frames fit\n",
                    host.provider->name, host.frame.count, host.out_w, host.out_h, (long long)(total / (1024 * 1024)),
                    host.frame.dt_ms, (long long)fits);
            return false;
        }
    }

#if defined(_WIN32)
    /* stdout is text mode by default on Windows, which would rewrite every
     * 0x0A pixel byte into a 0x0D 0x0A pair, silently corrupting the image
     * rather than failing loudly. */
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    if (!gfx_init()) {
        fprintf(stderr, "gfx_init failed\n");
        return false;
    }
    if (host.provider->setup != NULL && !host.provider->setup(host.frame.quarter)) {
        fprintf(stderr, "%s: setup failed\n", host.provider->name);
        return false;
    }

    if (gfx_readback_begin() != GFX_READBACK_READY) {
        /* Band mode keeps no retained frame to read back, so there is
         * nothing to write; the same refusal a device capture makes. A
         * provider wanting an image asks for the full-framebuffer layout. */
        fprintf(stderr, "no framebuffer to read: the scene left gfx in band mode\n");
        return false;
    }
    host.t = ui_transform_quarter_turn(host.frame.quarter, GFX_WIDTH, GFX_HEIGHT);
    host.stride = screenshot_bmp_row_stride(host.out_w);

    host.frame_buf = malloc((size_t)host.stride * host.out_h);
    if (host.frame_buf == NULL) {
        fprintf(stderr, "out of memory\n");
        return false;
    }

    if (host.video_path != NULL
        && !render_video_open(&host.video, host.video_path, host.out_w, host.out_h, host.frame.dt_ms)) {
        fprintf(stderr, "%s: cannot write %s\n", host.provider->name, host.video_path);
        free(host.frame_buf);
        return false;
    }

    /* stdout is free to watch only when the image goes to a file. */
    if (host.out_path != NULL) {
        host.console_path = malloc(strlen(host.out_path) + sizeof ".console");
        if (host.console_path != NULL) {
            sprintf(host.console_path, "%s.console", host.out_path);
        }
    }
    if (!render_watch_start(argv[0], host.console_path)) {
        fprintf(stderr, "%s: cannot capture stdout in %s\n", host.provider->name, host.console_path);
        free(host.console_path);
        free(host.frame_buf);
        return false;
    }

    /* The drawn box in output coordinates, and the map that turns a
     * --panel output's box back into the canvas a provider lays out in. */
    host.canvas_from_panel = affine2x_identity();
    (void)affine2x_invert(host.t, &host.canvas_from_panel);
    host.video_ok = true;
    return true;
}

void
render_host_step(uint32_t dt_ms, const input_t* input) {
    host.frame.dt_ms = dt_ms;
    host.frame.input = *input;
    render_watch_frame_begin();
    scene_shell_compose(host.frame.dt_ms); /* what the shell does before an app's frame() */
    host.provider->draw(&host.frame);
    render_watch_frame_end();

    convert_frame(host.t, host.panel, host.out_w, host.out_h, host.stride, host.frame_buf);
    if (host.crop) {
        host.drawn = drawn_box(host.frame_buf, host.stride, host.out_w, host.out_h);
        host.frame.drawn = host.panel ? ui_transform_rect(host.canvas_from_panel, host.drawn) : host.drawn;
    }
    gfx_present();
    if (host.video_path != NULL && host.video_ok) {
        host.video_ok = render_video_write_frame(&host.video, host.frame_buf, host.stride * host.out_h);
    }
    host.frame.index++;
    host.frame.elapsed_ms += dt_ms;
}

bool
render_host_close(void) {
    /* However few frames the image needed, the provider goes on drawing until
     * the watch has judged a whole window of it at rest. Nothing of these
     * reaches the image. */
    for (int i = host.frame.index; i < FRAME_WATCH_WARMUP + FRAME_WATCH_WINDOW; i++) {
        host.frame.index = i;
        apply_input(host.provider, i, &host.frame.input);
        render_watch_frame_begin();
        scene_shell_compose(host.frame.dt_ms); /* what the shell does before an app's frame() */
        host.provider->draw(&host.frame);
        gfx_present();
        render_watch_frame_end();
        host.frame.elapsed_ms += host.frame.dt_ms;
    }
    gfx_readback_end();

    const frame_watch_verdict_t verdict = render_watch_finish();
    free(host.console_path);

    if (host.video_path != NULL) {
        host.video_ok = render_video_close(&host.video) && host.video_ok;
        if (!host.video_ok) {
            fprintf(stderr, "%s: writing %s failed\n", host.provider->name, host.video_path);
            free(host.frame_buf);
            return false;
        }
    }

    FILE* out = stdout;
    if (host.out_path != NULL) {
        out = fopen(host.out_path, "wb");
        if (out == NULL) {
            fprintf(stderr, "%s: cannot write %s\n", host.provider->name, host.out_path);
            free(host.frame_buf);
            return false;
        }
    }

    render_size_t size = {0};
    recti_t box = {0, 0, host.out_w, host.out_h};
    if (host.crop) {
        box = crop_box(host.drawn, host.provider->crop_margin, host.out_w, host.out_h);
    }
    const bool ok = write_bmp(out, host.frame_buf, host.out_w, host.out_h, box, &size);
    free(host.frame_buf);
    if (out != stdout) {
        fclose(out);
        /* The shape the caller checks its own declaration against. Only for
         * a file: a render to stdout is read by a script that wants the
         * provider's own first stderr line, not this one. */
        fprintf(stderr, "RENDER %s %dx%d %ld\n", host.provider->name, size.width, size.height, size.bytes);
    }
    if (verdict.repeating > 0) {
        fprintf(stderr, "%s: %d call sites repeat frame after frame - see the FRAME_WATCH lines above\n",
                host.provider->name, verdict.repeating);
    }
    if (verdict.dropped > 0) {
        fprintf(stderr, "%s: the frame watch had no room for %lu events, so it may have missed one\n",
                host.provider->name, (unsigned long)verdict.dropped);
    }
    return ok && frame_watch_verdict_clean(verdict);
}

/* Selecting a provider builds the CLI; without it this file is a library. */
#ifdef RENDER_HOST_PROVIDER
extern const frame_provider_t RENDER_HOST_PROVIDER;

int
main(int argc, char** argv) {
    int exit_code;
    if (!render_host_open(&RENDER_HOST_PROVIDER, argc, argv, &exit_code)) {
        return exit_code;
    }
    input_t input = {0};
    for (int i = 0; i < host.frame.count; i++) {
        apply_input(host.provider, i, &input);
        render_host_step(host.frame.dt_ms, &input);
    }
    return render_host_close() ? 0 : 1;
}
#endif
