/*
 * render_lab_render_host - this app itself, entered and stepped on a host,
 * started on the scene --scene names.
 *
 * A render_host.h scene: the real enter()/frame(), the real gfx and ui
 * layers, no board, no IDF header, no clock or finger but the harness's own.
 * app_*.c is excluded from the host test runner as hardware-facing, but it
 * asks nothing of the board, so this links the real app_registry.c - its
 * own APP_REGISTER() constructor lands the one app that runs here - and
 * only display_shell_quarter() below stands in for the shell itself.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "apps/render_lab/render_lab_mesh_frame.h"
#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"
#include "util/screenshot.h"

/* The band ring keeps no retained frame for render_host.c to read back, so
 * setup() asks for the full-framebuffer layout. */
extern bool render_lab_band_mode;
extern bool render_lab_show_hud;
extern const char* render_lab_start_scene_key;

#define DEPTH_TILE  8
#define DEPTH_EMPTY 0

/* Where --depth writes, or NULL. */
static const char* depth_stem;
/* --holes: each frame's pinhole count on stderr. */
static bool report_holes;
static int shell_quarter;

/* Which names are valid is app_render_lab.c's own knowledge (each scene's
 * .key, render_lab_scene.h) - this only hands the string through. */
static bool
options(int argc, char** argv) {
    bool have_scene = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--no-hud") == 0) {
            render_lab_show_hud = false;
        } else if (strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
            render_lab_start_scene_key = argv[i + 1];
            have_scene = true;
            i++;
        } else if (strcmp(argv[i], "--depth") == 0 && i + 1 < argc) {
            depth_stem = argv[i + 1];
            i++;
        } else if (strcmp(argv[i], "--holes") == 0) {
            report_holes = true;
        }
    }
    if (!have_scene) {
        (void)fprintf(stderr, "render_lab_render_host needs --scene <key>\n");
    }
    return have_scene;
}

int
display_shell_quarter(void) {
    return shell_quarter;
}

static bool
setup(int quarter) {
    const app_t* registered = app_list();
    if (registered == NULL || registered->enter == NULL || registered->frame == NULL) {
        (void)fprintf(stderr, "no app registered itself\n");
        return false;
    }
    render_lab_band_mode = false;
    shell_quarter = quarter;
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    registered->enter();
    return true;
}

/* The farthest depth in the tile at (tx, ty): 65535 is nearest, so the
 * minimum. One empty pixel makes the whole tile empty. */
static uint16_t
tile_farthest(const uint16_t* depth, int w, int h, int tx, int ty) {
    uint16_t farthest = UINT16_MAX;
    for (int y = ty * DEPTH_TILE; y < (ty + 1) * DEPTH_TILE && y < h; y++) {
        for (int x = tx * DEPTH_TILE; x < (tx + 1) * DEPTH_TILE && x < w; x++) {
            const uint16_t d = depth[((size_t)y * w) + x];
            if (d == DEPTH_EMPTY) {
                return DEPTH_EMPTY;
            }
            farthest = d < farthest ? d : farthest;
        }
    }
    return farthest;
}

/* `out` is `depth` with every pixel replaced by its tile's farthest depth. */
static void
depth_tiles(const uint16_t* depth, int w, int h, uint16_t* out) {
    for (int ty = 0; ty * DEPTH_TILE < h; ty++) {
        for (int tx = 0; tx * DEPTH_TILE < w; tx++) {
            const uint16_t d = tile_farthest(depth, w, h, tx, ty);
            for (int y = ty * DEPTH_TILE; y < (ty + 1) * DEPTH_TILE && y < h; y++) {
                for (int x = tx * DEPTH_TILE; x < (tx + 1) * DEPTH_TILE && x < w; x++) {
                    out[((size_t)y * w) + x] = d;
                }
            }
        }
    }
}

/* Empty pixels whose four neighbours were all drawn: a pinhole in solid
 * geometry rather than sky, which comes in runs. */
static int
depth_holes(const uint16_t* depth, int w, int h) {
    int holes = 0;
    for (int y = 1; y + 1 < h; y++) {
        for (int x = 1; x + 1 < w; x++) {
            const uint16_t* p = depth + ((size_t)y * w) + x;
            if (p[0] == DEPTH_EMPTY && p[-1] != DEPTH_EMPTY && p[1] != DEPTH_EMPTY && p[-w] != DEPTH_EMPTY
                && p[w] != DEPTH_EMPTY) {
                holes++;
            }
        }
    }
    return holes;
}

/* The range of the pixels something was drawn to; false when none was. */
static bool
depth_range(const uint16_t* depth, size_t count, uint16_t* lo, uint16_t* hi) {
    bool any = false;
    *lo = UINT16_MAX;
    *hi = 0;
    for (size_t i = 0; i < count; i++) {
        if (depth[i] == DEPTH_EMPTY) {
            continue;
        }
        any = true;
        *lo = depth[i] < *lo ? depth[i] : *lo;
        *hi = depth[i] > *hi ? depth[i] : *hi;
    }
    return any;
}

/* Near bright, far dark, across the range this frame has; empty is flat
 * magenta, which no grey is. */
static void
depth_shade(uint16_t d, uint16_t lo, uint16_t hi, uint8_t rgb[3]) {
    if (d == DEPTH_EMPTY) {
        rgb[0] = 255;
        rgb[1] = 0;
        rgb[2] = 255;
        return;
    }
    const uint32_t span = (uint32_t)hi - lo;
    const uint8_t grey = span == 0 ? 255 : (uint8_t)((255U * (d - lo)) / span);
    rgb[0] = rgb[1] = rgb[2] = grey;
}

/* A BMP of `depth` turned to the way the scene is read at `quarter`, the
 * same rotation render_host.c gives the colour image. */
static bool
depth_write_bmp(const char* path, const uint16_t* depth, int w, int h, uint16_t lo, uint16_t hi, int quarter) {
    const bool upright = quarter % 2 == 0;
    const int out_w = upright ? w : h;
    const int out_h = upright ? h : w;
    const int32_t stride = screenshot_bmp_row_stride(out_w);
    uint8_t* body = malloc((size_t)stride * out_h);
    FILE* out = fopen(path, "wb");
    bool ok = body != NULL && out != NULL;
    if (ok) {
        const ui_transform_t t = ui_transform_quarter_turn(quarter, w, h);
        for (int y = 0; y < out_h; y++) {
            uint8_t* row = body + ((size_t)(out_h - 1 - y) * stride);
            for (int x = 0; x < out_w; x++) {
                const mu_Rect src = ui_transform_rect(t, (mu_Rect){x, y, 1, 1});
                uint8_t rgb[3];
                depth_shade(depth[((size_t)src.y * w) + src.x], lo, hi, rgb);
                row[(x * 3) + 0] = rgb[2];
                row[(x * 3) + 1] = rgb[1];
                row[(x * 3) + 2] = rgb[0];
            }
        }
        uint8_t header[SCREENSHOT_BMP_HEADER_SIZE];
        screenshot_bmp_header(header, out_w, out_h);
        ok = fwrite(header, 1, sizeof header, out) == sizeof header;
        ok = ok && fwrite(body, 1, (size_t)stride * out_h, out) == (size_t)stride * out_h;
    }
    if (out != NULL) {
        ok = fclose(out) == 0 && ok;
    }
    free(body);
    return ok;
}

/* `path` is the --depth stem plus `suffix`; false when it does not fit. */
static bool
stem_path(char* path, size_t size, const char* suffix) {
    const int n = snprintf(path, size, "%s-%s", depth_stem, suffix);
    return n > 0 && (size_t)n < size;
}

static bool
depth_write_all(const r3d_lit_frame_t* lit, int quarter) {
    const int w = lit->width;
    const int h = lit->height;
    const size_t count = (size_t)w * h;
    uint16_t* tiles = malloc(count * sizeof *tiles);
    if (tiles == NULL) {
        return false;
    }
    uint16_t lo;
    uint16_t hi;
    const bool drawn = depth_range(lit->depth, count, &lo, &hi);
    depth_tiles(lit->depth, w, h, tiles);

    char path[1024];
    bool ok = stem_path(path, sizeof path, "depth.bmp") && depth_write_bmp(path, lit->depth, w, h, lo, hi, quarter);
    ok = stem_path(path, sizeof path, "tiles.bmp") && depth_write_bmp(path, tiles, w, h, lo, hi, quarter) && ok;
    free(tiles);

    FILE* note = stem_path(path, sizeof path, "depth.txt") ? fopen(path, "w") : NULL;
    if (note == NULL) {
        return false;
    }
    const bool noted =
        fprintf(note, "depth %dx%d range %u..%u (65535 nearest, 0 empty, magenta) %s, tiles %dx%d px, holes %d\n", w, h,
                drawn ? (unsigned)lo : 0U, drawn ? (unsigned)hi : 0U, drawn ? "drawn" : "nothing drawn", DEPTH_TILE,
                DEPTH_TILE, depth_holes(lit->depth, w, h))
        > 0;
    return fclose(note) == 0 && noted && ok;
}

static void
draw(const render_frame_t* frame) {
    app_list()->frame(frame->dt_ms, &frame->input);
    const r3d_lit_frame_t* lit = render_lab_mesh_frame();
    if (report_holes && lit != NULL && frame->index < frame->count) {
        /* The scene's clock has already taken this frame's step. */
        (void)fprintf(stderr, "holes pose %u ms %d\n", (unsigned)(frame->elapsed_ms + frame->dt_ms),
                      depth_holes(lit->depth, lit->width, lit->height));
    }
    if (depth_stem == NULL || frame->index != frame->count - 1) {
        return;
    }
    if (lit == NULL) {
        (void)fprintf(stderr, "--depth: this scene keeps no mesh frame, no depth written\n");
    } else if (!depth_write_all(lit, frame->quarter)) {
        (void)fprintf(stderr, "--depth: cannot write %s-*\n", depth_stem);
    }
}

const render_scene_t render_scene = {
    .name = "render_lab",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
};
