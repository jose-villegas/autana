/*
 * asset_fixture: a render_host.h scene that mounts the pack its one argument
 * names, from the folder a scene's scene_assets built in, and refuses to
 * render without it. check_scene_assets.sh runs it.
 */

#include <stdio.h>

#include "asset/asset_store.h"
#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "render_host.h"

static bool
options(int argc, char** argv) {
    if (argc != 1) {
        fprintf(stderr, "this scene takes one argument: <pack>\n");
        return false;
    }
    const handle_t load = asset_store_load(argv[0]);
    const asset_pack_t* pack = asset_store_loaded(load);
    if (pack == NULL) {
        asset_store_release(load);
        return false;
    }
    printf("pack %s: %u entries\n", argv[0], (unsigned)pack->count);
    asset_store_release(load);
    return true;
}

static void
draw(const provider_frame_t* frame) {
    (void)frame;
    draw_clear(RGB565BE(0x000000));
}

const frame_provider_t asset_fixture_provider = {
    .name = "asset_fixture",
    .quarter = 0,
    .frames = 1,
    .options = options,
    .draw = draw,
};
