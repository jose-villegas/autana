/*
 * boot_anim_render_host, one frame of the boot animation, drawn by the
 * REAL firmware code (boot_anim.c + gfx/, unmodified) on a host build.
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
 *     main/gfx/                 (every .c but *_device.c; render_scene.sh)
 *     main/boot/boot_anim.c     (host-portable for the same reason)
 *     launcher_home_render_setup.sh (launcher sources and host setup)
 *
 * The camera and space it draws through are the boot clip's and the
 * photograph is the boot picture's, each loaded from its pack as boot does
 * (the rest pose, and no photograph, when there is none), and the timing is
 * boot_anim_timeline.h.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot/boot_anim.h"
#include "display/display.h"
#include "launcher_home_render_setup.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"

static uint32_t now_ms;
static boot_anim_motion_t motion;
static boot_anim_photo_t photo;

static bool
options(int argc, char** argv) {
    if (argc < 1) {
        fprintf(stderr, "usage: <now_ms> [--row <label> ...]\n");
        return false;
    }
    now_ms = (uint32_t)strtoul(argv[0], NULL, 10);
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--row") != 0 || i + 1 >= argc || !launcher_home_render_add_row(argv[++i])) {
            return false;
        }
    }
    launcher_home_render_register_rows();
    boot_anim_motion_load(&motion);
    boot_anim_photo_load(&photo);
    /* The fallbacks only when asked for, by pointing AUTANA_ASSET_DIR at a
     * folder without the packs: a renderer that lost its packs must fail. */
    if ((!motion.from_pack || !photo.from_pack) && getenv("AUTANA_ASSET_DIR") == NULL) {
        fprintf(stderr, "no boot clip or picture in the pack folder built into this renderer\n");
        return false;
    }
    return true;
}

/* shell_apps.c needs device services; these are its backdrop painter's two calls. */
static void
paint_home(void) {
    ui_invalidate();
    ui_launcher_frame(NULL, 0);
}

static bool
setup(int quarter) {
    (void)quarter;
    launcher_home_render_setup(DISPLAY_LANDSCAPE);
    /* microui clips to the previous frame's window rectangle. */
    paint_home();
    boot_anim_set_ending_backdrop(paint_home);
    return true;
}

static void
draw(const render_frame_t* frame) {
    const uint32_t t_ms = now_ms + frame->elapsed_ms;
    boot_anim_draw_frame(&motion, &photo.image, t_ms);
}

const render_scene_t render_scene = {
    .name = "boot_anim",
    .quarter = 0,
    .frames = 1,
    .options = options,
    .setup = setup,
    .draw = draw,
};
