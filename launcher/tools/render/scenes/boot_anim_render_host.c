/*
 * boot_anim_render_host - one frame of the boot animation, drawn by the
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
 *     main/gfx/gfx.c            (host-portable - see its own ESP_PLATFORM
 *                                comment)
 *     main/boot/boot_anim.c     (host-portable for the same reason)
 *
 * tools/boot_anim_editor_server.py compiles the same four with a scratch
 * directory holding a DRAFT boot_anim_timeline.h placed AHEAD of `main` on
 * the include path, so it shadows the real, committed one without ever
 * touching it - see that script's own top comment.
 *
 * Also prints one line to STDERR - "ORIGIN <x> <y>", the space's own local
 * origin projected through this frame's transform. It must stay the FIRST
 * stderr line: that script reads one line and stops.
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

    /* The space's own local origin (0,0,0 - t=0, zeta=0), projected through
     * this frame's camera+space transform: the JSON side has nowhere to
     * author a screen position directly, so boot_anim_editor_server.py reads
     * this as a read-only "where does the origin land" readout. */
    const boot_anim_view_t view = boot_anim_view(GFX_WIDTH, GFX_HEIGHT, t_ms);
    int ox, oy;
    boot_anim_project(0, 0, 0, &view, &ox, &oy);
    fprintf(stderr, "ORIGIN %d %d\n", ox, oy);
}

const render_scene_t render_scene = {
    .name = "boot_anim",
    .quarter = 0,
    .frames = 1,
    .options = options,
    .draw = draw,
};
