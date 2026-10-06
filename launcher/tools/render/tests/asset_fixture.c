/*
 * asset_fixture: a render_host.h scene that mounts the pack its one argument
 * names, from the folder a scene's scene_assets built in, and refuses to
 * render without it. check_scene_assets.sh runs it.
 */

#include <stdio.h>

#include "asset/asset_store.h"
#include "gfx/gfx.h"
#include "render_host.h"

static bool
options(int argc, char** argv) {
    if (argc != 1) {
        fprintf(stderr, "this scene takes one argument: <pack>\n");
        return false;
    }
    const asset_pack_t* pack = asset_store_pack(argv[0]);
    if (pack == NULL) {
        return false;
    }
    printf("pack %s: %u entries\n", argv[0], (unsigned)pack->count);
    asset_store_release(argv[0]);
    return true;
}

static void
draw(const render_frame_t* frame) {
    (void)frame;
    gfx_clear(GFX_RGB(0x000000));
}

const render_scene_t render_scene = {
    .name = "asset_fixture",
    .quarter = 0,
    .frames = 1,
    .options = options,
    .draw = draw,
};
