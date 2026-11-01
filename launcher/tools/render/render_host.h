/* Render real firmware drawing code on a host, to a BMP, with synthetic time
 * and input (no live board data); options are in docs/tools/Render-Harness.md.
 * Provider adapters supply shell stubs such as display_quarter_now and
 * shell_home_edge when linking hardware-facing screen entry code. */
#ifndef RENDER_HOST_H
#define RENDER_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "app/app.h"
#include "build/build_variant.h"
#include "math/geometry/recti.h"
#include "render/context/render_context.h"

/* What one frame of a provider is handed. `input` is synthesised from the
 * provider's declared touch steps, edges included. */
typedef struct {
    int index;
    int count;
    int quarter;
    uint32_t dt_ms;
    uint32_t elapsed_ms;
    input_t input;
    /* With --crop, the last frame's drawn box: its pixels unlike the colour
     * most of its border shares, in canvas coordinates (the UI's own, even
     * under --panel). Empty otherwise, before the first frame and for a
     * blank one. */
    recti_t drawn;
} provider_frame_t;

/* A declared touch sample, in PANEL coordinates, where a finger lands, not
 * where the rotated canvas puts it, exactly as the touch controller reports
 * it. It holds from `frame` until the next step, so a press and a release
 * are two entries, not one per frame. */
typedef struct {
    int frame;
    bool down;
    vec2i_t point;
} render_input_step_t;

typedef struct {
    const char* name;

    /* Defaults; the options above override each of them. */
    int quarter;
    int frames;
    uint32_t dt_ms;

    const render_input_step_t* input;
    int input_count;

    /* What --crop keeps round the last frame's drawn box, in pixels. */
    int crop_margin;

    /* The provider's own options, given every argument render_host.c did not
     * recognise. Return false to reject the command line. */
    bool (*options)(int argc, char** argv);

    /* Called once after gfx_init(), before the first frame. */
    bool (*setup)(int quarter);

    void (*draw)(const provider_frame_t* frame);
    /* Optional path duration in milliseconds; NULL for a provider without a path. */
    uint32_t (*path_ms)(void);
    /* Optional path cut: frame history starts over; NULL without a path. */
    void (*seek)(uint32_t t_ms);
    /* Optional: the render context the provider draws through, for a caller
     * that switches debug views; NULL for a provider that draws without one. */
    render_context_t* (*render_context)(void);
} frame_provider_t;

/* The open run's whole converted frame: bottom-up 24-bit BGR rows in the
 * BMP's row layout (--crop writes only a part of it). Valid until
 * render_host_close(). */
typedef struct {
    const uint8_t* bgr;
    int width, height;
    size_t stride; /* bytes from one row to the next, padding included */
} render_host_image_t;

render_host_image_t render_host_image(void);

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
typedef struct render_host_checkpoint render_host_checkpoint_t;
/* Holds the engine state the next frame reads: scene manager, render context, gfx and tunables.
 * Provider statics are excluded. Valid within the open run while captured allocations remain.
 * Timed choices (frame generation and predicted resolution) replay only as exactly as their clock. */
render_host_checkpoint_t* render_host_save(void);
bool render_host_restore(render_host_checkpoint_t* checkpoint);
void render_host_checkpoint_free(render_host_checkpoint_t* checkpoint);
/* The memory one checkpoint holds, in bytes. */
size_t render_host_checkpoint_bytes(const render_host_checkpoint_t* checkpoint);
#endif

/* Fully qualified tunable names use the same validation as the console. */
bool render_host_set_tunable(const char* name, int32_t value);
/* "owner.name=value", as --set takes it; false, said on stderr, for anything else. */
bool render_host_set_tunable_text(const char* assignment);

/* Parse options and prepare the provider, graphics and outputs for a run.
 * False means no run opened; exit_code is 0 for a successful query, nonzero
 * for an error. */
bool render_host_open(const frame_provider_t* provider, int argc, char** argv, int* exit_code);
/* The open run's frame: its count and dt as the options and the provider's
 * defaults set them, and its index so far. */
const provider_frame_t* render_host_frame(void);
/* Draw one composed frame; advance its index by one and elapsed time by dt_ms. */
void render_host_step(uint32_t dt_ms, const input_t* input);
/* Draw warm-up frames for the frame watch, write the saved image and close
 * outputs. False means the run failed, including a frame-watch violation. */
bool render_host_close(void);

#endif
