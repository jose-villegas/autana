/*
 * render_host - render REAL firmware drawing code on a host, to a BMP.
 *
 * One procedure, many scenes. A SCENE is a translation unit declaring the
 * `render_scene` below: what to draw, at which quarter turn, over how many
 * frames, and what synthetic touch to feed each one. render_host.c owns
 * main(), gfx_init() on a malloc framebuffer, the frame loop, the rotation
 * to the read orientation and the BMP encode from util/screenshot.h.
 *
 * Common options, parsed here, the rest handed to the scene's own
 * options():
 *
 *     --quarter N   as display.h numbers a quarter turn (default: declared)
 *     --panel       write the framebuffer the way the panel holds it, which
 *                   is the shape a device capture has; without it the image
 *                   comes out the way the board is READ at that quarter
 *     --frames N    how many frames to draw before the last one is written
 *     --dt N        milliseconds per frame
 *     -o PATH       output file (default: stdout)
 *
 * A scene stands in the data its screen normally gets - a fixture table, a
 * timestamp - so nothing rendered here is a reading from any board.
 */
#ifndef RENDER_HOST_H
#define RENDER_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "app.h"

/* What one frame of a scene is handed. `input` is synthesised from the
 * scene's declared touch steps, edges included. */
typedef struct {
    int index;
    int count;
    int quarter;
    uint32_t dt_ms;
    uint32_t elapsed_ms;
    input_t input;
} render_frame_t;

/* A declared touch sample, in the LOGICAL canvas the scene draws in. It
 * holds from `frame` until the next step, so a press and a release are two
 * entries, not one per frame. */
typedef struct {
    int frame;
    bool down;
    int x, y;
} render_input_step_t;

typedef struct {
    const char* name;

    /* Defaults; the options above override each of them. */
    int quarter;
    int frames;
    uint32_t dt_ms;

    const render_input_step_t* input;
    int input_count;

    /* The scene's own options, given every argument render_host.c did not
     * recognise. Return false to reject the command line. */
    bool (*options)(int argc, char** argv);

    /* Called once after gfx_init(), before the first frame. */
    bool (*setup)(int quarter);

    void (*draw)(const render_frame_t* frame);
} render_scene_t;

/* Every scene defines exactly one of these. */
extern const render_scene_t render_scene;

#endif
