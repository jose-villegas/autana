/* mip_figures_probe: a receding floor drawn through the span fill with a
 * texture whose every mip level is one solid colour, so each pixel's colour
 * names the level its triangle picked; beside it, each pixel's own level
 * from the texels one screen step moves there. Prints both maps as JSON. */
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "collections/bytes_write.h"
#include "render/r3d_span.h"
#include "render/texture/r3d_texture.h"

enum {
    WIDTH = 320,
    HEIGHT = 200,
    SIZE_LOG2 = 6, /* 64 texels a side: six levels down to 2 */
    LEVELS = SIZE_LOG2 - R3D_TEXTURE_SIZE_LOG2_MIN + 1,
    NONE = -1,
    TEXEL_BITS = CHAR_BIT / R3D_TEXTURE_INDEX4_PER_BYTE,
};

/* The camera: its height over the floor, focal length in pixels, and the
 * horizon's row; the floor's tiles, one texture a tile. */
static const float EYE_HEIGHT = 1.6F;
static const float FOCAL_PX = 220.0F;
static const float HORIZON_ROW = 20.0F;
static const float TILE = 1.0F;
static const float NEAR_Z = 1.6F;
static const float FAR_Z = 48.0F;
static const float HALF_WIDTH = 6.0F;
/* Inverse depth a vertex carries, (0, 1]: 1 at this depth. */
static const float UNIT_Z = 1.0F;
static const float FULL_LIGHT = 255.0F;

static uint16_t color[HEIGHT][WIDTH];
static uint16_t depth[HEIGHT][WIDTH];

/* An INDEX4 texture whose level l is palette colour l throughout; colour
 * i's channels are spread apart so every colour lights to its own RGB565. */
static uint8_t*
solid_levels_ltex(size_t* bytes) {
    const size_t palette_off = R3D_TEXTURE_AT_TEXELS_OFF + sizeof(uint32_t); /* after the texels offset */
    const size_t texels_off = palette_off + (size_t)R3D_TEXTURE_COLOURS * R3D_TEXTURE_PALETTE_STRIDE;
    size_t texels = 0;
    for (int l = 0; l < LEVELS; l++) {
        texels += r3d_texture_level_bytes(R3D_TEXTURE_INDEX4, SIZE_LOG2 - l);
    }
    *bytes = texels_off + texels;
    uint8_t* b = calloc(1, *bytes);
    if (b == NULL) {
        return NULL;
    }
    bytes_write_u16(b + R3D_TEXTURE_AT_VERSION, R3D_TEXTURE_VERSION);
    bytes_write_u16(b + R3D_TEXTURE_AT_FORMAT, R3D_TEXTURE_INDEX4);
    b[R3D_TEXTURE_AT_SIZE_LOG2] = SIZE_LOG2;
    b[R3D_TEXTURE_AT_LEVELS] = LEVELS;
    memset(b + R3D_TEXTURE_AT_TINT, UINT8_MAX, R3D_TEXTURE_TINT_RGB);
    bytes_write_u32(b + R3D_TEXTURE_AT_PALETTE_OFF, (uint32_t)palette_off);
    bytes_write_u32(b + R3D_TEXTURE_AT_TEXELS_OFF, (uint32_t)texels_off);
    for (int i = 0; i < R3D_TEXTURE_COLOURS; i++) {
        uint8_t* rgb = b + palette_off + ((size_t)i * R3D_TEXTURE_PALETTE_STRIDE);
        rgb[0] = (uint8_t)(R3D_TEXTURE_ONE - (i * 7));
        rgb[1] = (uint8_t)(R3D_TEXTURE_ONE / 2 + (i * 5));
        rgb[2] = (uint8_t)(i * 8);
    }
    uint8_t* at = b + texels_off;
    for (int l = 0; l < LEVELS; l++) {
        const size_t n = r3d_texture_level_bytes(R3D_TEXTURE_INDEX4, SIZE_LOG2 - l);
        memset(at, (int)(l | (l << TEXEL_BITS)), n);
        at += n;
    }
    return b;
}

/* The floor point under pixel centre (px, py), in tiles of u and v texels;
 * false above the horizon. */
static int
floor_uv(float px, float py, float* u, float* v) {
    const float below = py - HORIZON_ROW;
    if (below <= 0.0F) {
        return 0;
    }
    const float z = FOCAL_PX * EYE_HEIGHT / below;
    const float x = (px - (WIDTH / 2.0F)) * z / FOCAL_PX;
    const float texels = (float)(1 << SIZE_LOG2) / TILE;
    *u = x * texels;
    *v = z * texels;
    return 1;
}

/* The level a pixel's own step reads: the rule of r3d_span_textured.c's pick,
 * on the texels one screen step moves at this pixel. */
static int
pixel_level(int x, int y) {
    float u0, v0, ux, vx, uy, vy;
    if (!floor_uv((float)x + 0.5F, (float)y + 0.5F, &u0, &v0) || !floor_uv((float)x + 1.5F, (float)y + 0.5F, &ux, &vx)
        || !floor_uv((float)x + 0.5F, (float)y + 1.5F, &uy, &vy)) {
        return NONE;
    }
    const float rho2 =
        fmaxf(((ux - u0) * (ux - u0)) + ((vx - v0) * (vx - v0)), ((uy - u0) * (uy - u0)) + ((vy - v0) * (vy - v0)));
    int level = 0;
    while (level + 1 < LEVELS && rho2 >= (float)(1U << (2 * (level + 1)))) {
        level++;
    }
    return level;
}

static r3d_span_vertex_t
project(float x, float z) {
    const float px = (WIDTH / 2.0F) + (FOCAL_PX * x / z);
    const float py = HORIZON_ROW + (FOCAL_PX * EYE_HEIGHT / z);
    return (r3d_span_vertex_t){(int32_t)lrintf(px * R3D_SUBPIXEL),
                               (int32_t)lrintf(py * R3D_SUBPIXEL),
                               UNIT_Z / z,
                               FULL_LIGHT,
                               FULL_LIGHT,
                               FULL_LIGHT};
}

static void
draw_floor(const r3d_span_target_t* target, const r3d_texture_t* texture) {
    const float texels = (float)(1 << SIZE_LOG2) / TILE;
    for (float z = NEAR_Z; z < FAR_Z; z += TILE) {
        for (float x = -HALF_WIDTH; x < HALF_WIDTH; x += TILE) {
            const float x1 = x + TILE;
            const float z1 = z + TILE;
            const r3d_span_vertex_t a = project(x, z), b = project(x1, z), c = project(x1, z1), d = project(x, z1);
            const vec2f_t ua = {x * texels, z * texels}, ub = {x1 * texels, z * texels},
                          uc = {x1 * texels, z1 * texels}, ud = {x * texels, z1 * texels};
            const vec2f_t first[3] = {ua, ub, uc};
            const vec2f_t second[3] = {ua, uc, ud};
            r3d_span_textured_triangle(target, &a, &b, &c, first, texture);
            r3d_span_textured_triangle(target, &a, &c, &d, second, texture);
        }
    }
}

/* The level a drawn colour names, NONE where nothing was drawn. */
static int
level_of(const r3d_texture_t* texture, uint16_t drawn) {
    for (int row = 0; row < R3D_TEXTURE_LIGHT_LEVELS; row++) {
        for (int l = 0; l < LEVELS; l++) {
            if (texture->colormap[row][l] == drawn) {
                return l;
            }
        }
    }
    return NONE;
}

static void
print_map(const char* name, int (*at)(int, int, const r3d_texture_t*), const r3d_texture_t* texture) {
    printf(",\"%s\":[", name);
    for (int y = 0; y < HEIGHT; y++) {
        for (int x = 0; x < WIDTH; x++) {
            printf("%s%d", (x | y) ? "," : "", at(x, y, texture));
        }
    }
    printf("]");
}

static int
picked_at(int x, int y, const r3d_texture_t* texture) {
    return depth[y][x] == R3D_DEPTH_EMPTY ? NONE : level_of(texture, color[y][x]);
}

static int
ideal_at(int x, int y, const r3d_texture_t* texture) {
    (void)texture;
    return depth[y][x] == R3D_DEPTH_EMPTY ? NONE : pixel_level(x, y);
}

int
main(void) {
    size_t bytes = 0;
    uint8_t* ltex = solid_levels_ltex(&bytes);
    r3d_texture_t texture;
    const asset_view_t view = {ltex, bytes};
    if (ltex == NULL || r3d_texture_from_asset(&view, &texture) != ASSET_OK) {
        fprintf(stderr, "mip_figures_probe: the solid-level texture did not open\n");
        return 1;
    }
    const r3d_span_target_t target = r3d_span_target(&color[0][0], &depth[0][0], WIDTH, 0, HEIGHT);
    draw_floor(&target, &texture);
    printf("{\"width\":%d,\"height\":%d,\"levels\":%d,\"texels\":%d", WIDTH, HEIGHT, LEVELS, 1 << SIZE_LOG2);
    print_map("picked", picked_at, &texture);
    print_map("ideal", ideal_at, &texture);
    printf("}\n");
    free(ltex);
    return 0;
}
