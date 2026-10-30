/*
 * render_host: the shared host render procedure. Real scene code draws into
 * gfx-owned pictures; gfx_read_panel_row() supplies completed output.
 * BMP and AVI encoding use gfx/capture/screenshot.h. No device access.
 *
 * --serve draws on demand instead, one command per stdin line, each answered
 * on stdout; anything else the process prints goes to stderr:
 *
 *   frame               draws a frame: "frame w h stride us", us the draw's time,
 *                       then stride * h bytes of bottom-up 24-bit BGR rows
 *   set owner.name=V    sets a tunable: "ok" or "error ..."
 *   tunables PREFIX     "ok" then " name=value:low:high" per tunable named PREFIX...
 *   quit                ends the process
 *
 * and the scene's own commands (render_scene_t.command).
 */

#include "render_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "console/console_tune.h"
#include "core/timing.h"
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

#include "scene/scene_shell.h"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include "image/pixel/rgb565.h"
#else
#include <unistd.h>
#endif

#define DEFAULT_DT_MS   16

/* RIFF AVI 1.0 keeps its whole size in a 32-bit field; players choke well
 * before that wraps, so a run past this is refused instead of written. */
#define VIDEO_MAX_BYTES ((int64_t)1024 * 1024 * 1024)

/* The longest command --serve reads and reply it writes. */
#define SERVE_LINE_MAX  256
#define SERVE_REPLY_MAX 4096

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

/* The scene manager's compose where a scene did not link it: nothing to
 * compose. scene/scene_shell.c's own definition replaces this. */
__attribute__((weak)) void
scene_shell_compose(uint32_t dt_ms) {
    (void)dt_ms;
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
            state->point = step->point;
        }
    }

    state->pressed = state->down && !was_down;
    state->released = !state->down && was_down;
    if (state->pressed) {
        state->press = state->point;
    }
}

/* The stdout the process started with, for --serve's replies alone: from
 * here on stdout is stderr, so nothing else printed lands among the frames. */
static FILE*
serve_channel(void) {
    (void)fflush(stdout);
#if defined(_WIN32)
    const int channel = _dup(_fileno(stdout));
    if (channel < 0 || _dup2(_fileno(stderr), _fileno(stdout)) != 0) {
        return NULL;
    }
    (void)_setmode(channel, _O_BINARY);
    return _fdopen(channel, "wb");
#else
    const int channel = dup(STDOUT_FILENO);
    if (channel < 0 || dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
        return NULL;
    }
    return fdopen(channel, "wb");
#endif
}

/* `line` past `command` and its space, or NULL when it is another command. */
static const char*
argument_of(const char* line, const char* command) {
    const size_t length = strlen(command);
    return strncmp(line, command, length) == 0 && line[length] == ' ' ? line + length + 1 : NULL;
}

/* "ok", then " name=value:low:high" for each tunable whose name starts with `prefix`. */
static void
tunables_reply(const char* prefix, char* reply, size_t size) {
    int used = snprintf(reply, size, "ok");
    for (const tune_entry_t* e = tune_shared()->first; e != NULL && used >= 0 && (size_t)used < size; e = e->next) {
        if (strncmp(e->name, prefix, strlen(prefix)) == 0) {
            used += snprintf(reply + used, size - (size_t)used, " %s=%ld:%ld:%ld", e->name, (long)*e->value,
                             (long)e->low, (long)e->high);
        }
    }
}

/* --serve: draws when asked, until "quit" or the end of stdin. */
static int
serve(const render_scene_t* scene, render_frame_t* frame, affine2x_t t, bool panel, int out_w, int out_h,
      int32_t stride, uint8_t* frame_buf) {
    FILE* channel = serve_channel();
    if (channel == NULL) {
        fprintf(stderr, "%s: --serve cannot keep stdout for its replies\n", scene->name);
        return 1;
    }
    char line[SERVE_LINE_MAX];
    char reply[SERVE_REPLY_MAX];
    while (fgets(line, sizeof line, stdin) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        const char* argument;
        if (strcmp(line, "quit") == 0) {
            break;
        }
        if (strcmp(line, "frame") == 0) {
            const int64_t start_us = timing_now_us();
            scene_shell_compose(0); /* the scene as it stands: no time passes */
            scene->draw(frame);
            const int64_t draw_us = timing_now_us() - start_us;
            convert_frame(t, panel, out_w, out_h, stride, frame_buf);
            gfx_present();
            frame->index++;
            fprintf(channel, "frame %d %d %ld %lld\n", out_w, out_h, (long)stride, (long long)draw_us);
            (void)fwrite(frame_buf, (size_t)stride, (size_t)out_h, channel);
        } else {
            if ((argument = argument_of(line, "set")) != NULL) {
                if (render_host_set_tunable_text(argument)) {
                    (void)snprintf(reply, sizeof reply, "ok");
                } else {
                    (void)snprintf(reply, sizeof reply, "error cannot set %s", argument);
                }
            } else if ((argument = argument_of(line, "tunables")) != NULL) {
                tunables_reply(argument, reply, sizeof reply);
            } else if (scene->command == NULL || !scene->command(line, reply, sizeof reply)) {
                (void)snprintf(reply, sizeof reply, "error unknown command: %s", line);
            }
            fprintf(channel, "%s\n", reply);
        }
        (void)fflush(channel);
    }
    (void)fclose(channel);
    return 0;
}

static int
usage(const char* argv0) {
    fprintf(stderr,
            "usage: %s [--quarter N] [--panel] [--frames N] [--dt N] [-o PATH] [--video PATH] [--serve]\n"
            "       [--set owner.name=value ...] [--tunables] [--crop]\n"
            "       docs/tools/Render-Harness.md lists options; the scene may take more.\n",
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
    bool crop = false;
    bool serving = false;

    char* rest[64];
    int rest_count = 0;

    for (int i = 1; i < argc; i++) {
        const char* a = argv[i];
        const bool has_value = i + 1 < argc;
        if (strcmp(a, "--panel") == 0) {
            panel = true;
        } else if (strcmp(a, "--quarter") == 0 && has_value) {
            quarter = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--tunables") == 0) {
            (void)console_tune_line(tune_shared(), "TUNE", tune_reply);
            return 0;
        } else if (strcmp(a, "--set") == 0 && has_value) {
            if (!render_host_set_tunable_text(argv[++i])) {
                return usage(argv[0]);
            }
        } else if (strcmp(a, "--frames") == 0 && has_value) {
            frames = (int)strtol(argv[++i], NULL, 10);
        } else if (strcmp(a, "--dt") == 0 && has_value) {
            dt_ms = (uint32_t)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(a, "-o") == 0 && has_value) {
            out_path = argv[++i];
        } else if (strcmp(a, "--video") == 0 && has_value) {
            video_path = argv[++i];
        } else if (strcmp(a, "--crop") == 0) {
            crop = true;
        } else if (strcmp(a, "--serve") == 0) {
            serving = true;
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
     * 0x0A pixel byte into a 0x0D 0x0A pair, silently corrupting the image
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

    if (gfx_readback_begin() != GFX_READBACK_READY) {
        /* Band mode keeps no retained frame to read back, so there is
         * nothing to write; the same refusal a device capture makes. A
         * scene wanting an image asks for the full-framebuffer layout. */
        fprintf(stderr, "no framebuffer to read: the scene left gfx in band mode\n");
        return 1;
    }
    const affine2x_t t = ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT);
    const int32_t stride = screenshot_bmp_row_stride(out_w);

    uint8_t* frame_buf = malloc((size_t)stride * out_h);
    if (frame_buf == NULL) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }

    if (serving) {
        render_frame_t served = {.count = 1, .quarter = quarter};
        const int code = serve(scene, &served, t, panel, out_w, out_h, stride, frame_buf);
        gfx_readback_end();
        free(frame_buf);
        return code;
    }

    render_video_t video;
    if (video_path != NULL && !render_video_open(&video, video_path, out_w, out_h, dt_ms)) {
        fprintf(stderr, "%s: cannot write %s\n", scene->name, video_path);
        free(frame_buf);
        return 1;
    }

    /* stdout is free to watch only when the image goes to a file. */
    char* console_path = NULL;
    if (out_path != NULL) {
        console_path = malloc(strlen(out_path) + sizeof ".console");
        if (console_path != NULL) {
            sprintf(console_path, "%s.console", out_path);
        }
    }
    if (!render_watch_start(argv[0], console_path)) {
        fprintf(stderr, "%s: cannot capture stdout in %s\n", scene->name, console_path);
        free(console_path);
        free(frame_buf);
        return 1;
    }

    render_frame_t frame = {.count = frames, .quarter = quarter, .dt_ms = dt_ms};
    /* The drawn box in output coordinates, and the map that turns a
     * --panel output's box back into the canvas a scene lays out in. */
    recti_t drawn = {0, 0, 0, 0};
    affine2x_t canvas_from_panel = affine2x_identity();
    (void)affine2x_invert(t, &canvas_from_panel);
    bool video_ok = true;
    for (int i = 0; i < frames; i++) {
        frame.index = i;
        frame.elapsed_ms = (uint32_t)i * dt_ms;
        apply_input(scene, i, &frame.input);
        render_watch_frame_begin();
        scene_shell_compose(frame.dt_ms); /* what the shell does before an app's frame() */
        scene->draw(&frame);
        render_watch_frame_end();

        convert_frame(t, panel, out_w, out_h, stride, frame_buf);
        if (crop) {
            drawn = drawn_box(frame_buf, stride, out_w, out_h);
            frame.drawn = panel ? ui_transform_rect(canvas_from_panel, drawn) : drawn;
        }
        gfx_present();
        if (video_path != NULL && video_ok) {
            video_ok = render_video_write_frame(&video, frame_buf, stride * out_h);
        }
    }

    /* However few frames the image needed, the scene goes on drawing until
     * the watch has judged a whole window of it at rest. Nothing of these
     * reaches the image. */
    for (int i = frames; i < FRAME_WATCH_WARMUP + FRAME_WATCH_WINDOW; i++) {
        frame.index = i;
        frame.elapsed_ms = (uint32_t)i * dt_ms;
        apply_input(scene, i, &frame.input);
        render_watch_frame_begin();
        scene_shell_compose(frame.dt_ms); /* what the shell does before an app's frame() */
        scene->draw(&frame);
        gfx_present();
        render_watch_frame_end();
    }
    gfx_readback_end();

    const frame_watch_verdict_t verdict = render_watch_finish();
    free(console_path);

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
    recti_t box = {0, 0, out_w, out_h};
    if (crop) {
        box = crop_box(drawn, scene->crop_margin, out_w, out_h);
    }
    const bool ok = write_bmp(out, frame_buf, out_w, out_h, box, &size);
    free(frame_buf);
    if (out != stdout) {
        fclose(out);
        /* The shape the caller checks its own declaration against. Only for
         * a file: a render to stdout is read by a script that wants the
         * scene's own first stderr line, not this one. */
        fprintf(stderr, "RENDER %s %dx%d %ld\n", scene->name, size.width, size.height, size.bytes);
    }
    if (verdict.repeating > 0) {
        fprintf(stderr, "%s: %d call sites repeat frame after frame - see the FRAME_WATCH lines above\n", scene->name,
                verdict.repeating);
    }
    if (verdict.dropped > 0) {
        fprintf(stderr, "%s: the frame watch had no room for %lu events, so it may have missed one\n", scene->name,
                (unsigned long)verdict.dropped);
    }
    return ok && frame_watch_verdict_clean(verdict) ? 0 : 1;
}
