/* Render real firmware drawing code on a host, to a BMP, with synthetic time
 * and input (no live board data); options are in docs/tools/Render-Harness.md.
 * Scene adapters supply shell stubs such as display_quarter_now and
 * shell_home_edge when linking hardware-facing screen entry code. */
#ifndef RENDER_HOST_H
#define RENDER_HOST_H

#include <stdbool.h>
#include <stdint.h>

#include "app/app.h"

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

/* A declared touch sample, in PANEL coordinates, where a finger lands, not
 * where the rotated canvas puts it, exactly as the touch controller reports
 * it. It holds from `frame` until the next step, so a press and a release
 * are two entries, not one per frame. */
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

/* Fully qualified tunable names use the same validation as the console. */
bool render_host_set_tunable(const char* name, int32_t value);

/* Every scene defines exactly one of these. */
extern const render_scene_t render_scene;

#endif
