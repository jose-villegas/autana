/*
 * boot_anim_render_host, one frame of the boot animation, drawn by the
 * REAL firmware code (boot_anim.c + gfx.c, unmodified) on a host build.
 *
 *     boot_anim_render_host <now_ms>   > frame.bmp
 *
 * <now_ms> is where the FIRST frame draws; --frames > 1 (--video, say)
 * advances it by the harness's own elapsed_ms per frame, so the timeline
 * animates instead of holding on one instant.
 *
 * A render_host.h scene; render_host.c owns main(), gfx_init() and the BMP.
 * Built from these translation units, with `main` on the include path:
 *
 *     boot_anim_render_host.c   (this file)
 *     render_host.c
 *     main/gfx/gfx.c            (host-portable, see its own ESP_PLATFORM
 *                                comment)
 *     main/boot/boot_anim.c     (host-portable for the same reason)
 *
 * The camera and space it draws through are the baked tracks in
 * main/boot/boot_anim_tracks_generated.c, and the timing is
 * boot_anim_timeline.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "boot/boot_anim.h"
#include "gfx/gfx.h"
#include "render_host.h"

static uint32_t now_ms;

static bool
options(int argc, char** argv) {
    if (argc != 1) {
        fprintf(stderr, "this scene takes one argument: <now_ms>\n");
        return false;
    }
    now_ms = (uint32_t)strtoul(argv[0], NULL, 10);
    return true;
}

static void
draw(const render_frame_t* frame) {
    const uint32_t t_ms = now_ms + frame->elapsed_ms;
    boot_anim_draw_frame(t_ms);
}

const render_scene_t render_scene = {
    .name = "boot_anim",
    .quarter = 0,
    .frames = 1,
    .options = options,
    .draw = draw,
};
