/* Arena: the shell reclaims app memory after exit. */
#include "app/app_arena.h"
#include "gfx/target/gfx_color.h"

#define BUF_BYTES (1024 * sizeof(gfx_color_t))

static gfx_color_t* buf;

static void
arena_enter(void) {
    buf = app_arena_take(BUF_BYTES, _Alignof(gfx_color_t)); /* NULL: run without it */
}

static void
arena_exit(void) {
    buf = NULL; /* nothing to free */
}
