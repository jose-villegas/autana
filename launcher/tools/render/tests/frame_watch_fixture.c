/*
 * frame_watch_fixture - a scene that allocates or prints on every frame, or
 * on one, so check_frame_watch.sh can see the render harness fail the first
 * and pass the second.
 *
 *     --alloc every|once   malloc() and free() a block
 *     --print every|once   printf() a line
 */
#include "render_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"

typedef enum { NEVER, ONCE, EVERY } how_often_t;

static how_often_t alloc_when;
static how_often_t print_when;

static bool
parse_how_often(const char* word, how_often_t* out) {
    if (strcmp(word, "every") == 0) {
        *out = EVERY;
        return true;
    }
    if (strcmp(word, "once") == 0) {
        *out = ONCE;
        return true;
    }
    return false;
}

static bool
options(int argc, char** argv) {
    for (int i = 0; i + 1 < argc; i += 2) {
        how_often_t* target = strcmp(argv[i], "--alloc") == 0   ? &alloc_when
                              : strcmp(argv[i], "--print") == 0 ? &print_when
                                                                : NULL;
        if (target == NULL || !parse_how_often(argv[i + 1], target)) {
            return false;
        }
    }
    return argc % 2 == 0;
}

static bool
due(how_often_t when, const render_frame_t* frame) {
    return when == EVERY || (when == ONCE && frame->index == frame->count / 2);
}

static void
draw(const render_frame_t* frame) {
    if (due(alloc_when, frame)) {
        volatile char* block = malloc(64);
        if (block != NULL) {
            block[0] = (char)frame->index;
        }
        free((void*)block);
    }
    if (due(print_when, frame)) {
        printf("fixture frame %d\n", frame->index);
    }
    gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x203040));
}

const render_scene_t render_scene = {
    .name = "frame_watch_fixture",
    .frames = 40,
    .dt_ms = 16,
    .options = options,
    .draw = draw,
};
