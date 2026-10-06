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
 * The camera and space it draws through are the boot clip's, loaded from its
 * pack as boot does (the rest pose when there is none), and the timing is
 * boot_anim_timeline.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "boot/boot_anim.h"
#include "gfx/gfx.h"
#include "render_host.h"

static uint32_t now_ms;
static boot_anim_motion_t motion;

static bool
options(int argc, char** argv) {
    if (argc != 1) {
        fprintf(stderr, "this scene takes one argument: <now_ms>\n");
        return false;
    }
    now_ms = (uint32_t)strtoul(argv[0], NULL, 10);
    boot_anim_motion_load(&motion);
    /* The rest pose only when asked for, by pointing AUTANA_ASSET_DIR at a
     * folder without the clip: a renderer that lost its packs must fail. */
    if (!motion.from_pack && getenv("AUTANA_ASSET_DIR") == NULL) {
        fprintf(stderr, "no boot clip in the pack folder built into this renderer\n");
        return false;
    }
    return true;
}

static void
draw(const render_frame_t* frame) {
    const uint32_t t_ms = now_ms + frame->elapsed_ms;
    boot_anim_draw_frame(&motion, t_ms);
}

const render_scene_t render_scene = {
    .name = "boot_anim",
    .quarter = 0,
    .frames = 1,
    .options = options,
    .draw = draw,
};
