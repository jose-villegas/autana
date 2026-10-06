/*
 * frame_watch_fixture, a scene doing one kind of heap or stdout work on
 * every frame, on one, or spread over three call sites taking turns, so
 * check_frame_watch.sh can see what the render harness fails and passes.
 *
 *     --malloc | --calloc | --realloc | --free | --print   every|once|turns
 *
 * `turns` rotates the work across three call sites of its own; a print has
 * no call site on a host, so it takes no turns. `once` lands after the
 * warm-up, where it is judged. setup() and the last declared frame print a
 * line each, to check stdout comes back in order.
 */
#include "render_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx/gfx.h"
#include "util/runtime/frame_watch.h"

typedef enum { NEVER, ONCE, EVERY, TURNS } how_often_t;

typedef enum { MALLOC, CALLOC, REALLOC, FREE, PRINT, WORK_KINDS } work_t;

static const char* const work_flags[WORK_KINDS] = {"--malloc", "--calloc", "--realloc", "--free", "--print"};

static how_often_t work_when[WORK_KINDS];

#define ONCE_AT_FRAME (FRAME_WATCH_WARMUP + FRAME_WATCH_WINDOW / 2)

/* --free frees one of these per frame; setup() allocates them all. */
#define FREE_BLOCKS   (FRAME_WATCH_WARMUP + FRAME_WATCH_WINDOW)
static void* free_blocks[FREE_BLOCKS];
static int freed;

static bool
parse_how_often(const char* word, how_often_t* out) {
    static const char* const words[] = {"never", "once", "every", "turns"};
    for (int i = 0; i < (int)(sizeof words / sizeof words[0]); i++) {
        if (strcmp(word, words[i]) == 0) {
            *out = (how_often_t)i;
            return true;
        }
    }
    return false;
}

static bool
options(int argc, char** argv) {
    if (argc % 2 != 0) {
        return false;
    }
    for (int i = 0; i < argc; i += 2) {
        int work = 0;
        while (work < WORK_KINDS && strcmp(argv[i], work_flags[work]) != 0) {
            work++;
        }
        if (work == WORK_KINDS || !parse_how_often(argv[i + 1], &work_when[work])) {
            return false;
        }
    }
    return true;
}

static bool
setup(int quarter) {
    (void)quarter;
    for (int i = 0; i < FREE_BLOCKS; i++) {
        free_blocks[i] = malloc(16);
    }
    printf("fixture setup\n");
    return true;
}

/* Written to, or the compiler may drop a malloc() freed straight away.
 * Inlined, so each caller frees from a call site of its own. */
static inline __attribute__((always_inline)) void
touch_and_free(void* block) {
    volatile char* bytes = block;
    if (bytes != NULL) {
        bytes[0] = 1;
    }
    free(block);
}

static __attribute__((noinline)) void
malloc_here_0(void) {
    touch_and_free(malloc(48));
}

static __attribute__((noinline)) void
malloc_here_1(void) {
    touch_and_free(malloc(48));
}

static __attribute__((noinline)) void
malloc_here_2(void) {
    touch_and_free(malloc(48));
}

static void
do_malloc(int turn) {
    switch (turn) {
        case 0: malloc_here_0(); break;
        case 1: malloc_here_1(); break;
        default: malloc_here_2(); break;
    }
}

static void
do_calloc(void) {
    touch_and_free(calloc(8, 8));
}

/* Grows one block without ever freeing it, so only realloc() repeats. */
static void
do_realloc(void) {
    static void* grown;
    static size_t size = 16;
    size = size >= 4096 ? 16 : size * 2;
    void* moved = realloc(grown, size);
    if (moved != NULL) {
        grown = moved;
    }
}

static void
do_free(void) {
    if (freed < FREE_BLOCKS) {
        free(free_blocks[freed++]);
    }
}

static void
do_work(work_t work, int turn, int index) {
    switch (work) {
        case MALLOC: do_malloc(turn); break;
        case CALLOC: do_calloc(); break;
        case REALLOC: do_realloc(); break;
        case FREE: do_free(); break;
        case PRINT: printf("fixture frame %d\n", index); break;
        case WORK_KINDS: break;
    }
}

static void
draw(const render_frame_t* frame) {
    for (int work = 0; work < WORK_KINDS; work++) {
        const how_often_t when = work_when[work];
        const bool due = when == EVERY || when == TURNS || (when == ONCE && frame->index == ONCE_AT_FRAME);
        if (due) {
            do_work((work_t)work, when == TURNS ? frame->index % 3 : 0, frame->index);
        }
    }
    if (frame->index == frame->count - 1) {
        printf("fixture last frame\n");
    }
    gfx_fill_rect(0, 0, GFX_WIDTH, GFX_HEIGHT, gfx_rgb(0x203040));
}

const render_scene_t render_scene = {
    .name = "frame_watch_fixture",
    .frames = 4,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
};
