#include "render/view/raster_lines.h"

#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "math/projection/projectf.h"
#include "math/scalar/mathf.h"
#include "math/scalar/mathi.h"
#include "render/r3d_span.h"
#include "render/raster_internal.h"

/* A box's corners, box3f_corner()'s k, each bit an axis. */
#define BOX_CORNERS  8
/* A box's edges lie on the surfaces they bound: a point counts as behind
 * only past this share of the drawn depth. */
#define BEHIND_SLACK 0.02F
/* The depth samples either side of a point that the test reads. */
#define NEIGHBOURS   1

void
raster_lines_depth(raster_lines_depth_t* depth, const raster_t* raster, int width, int height, unsigned behind_keep) {
    const raster_buffers_t b = raster_carve(raster);
    *depth =
        (raster_lines_depth_t){b.picture.attachment[GFX_ATTACHMENT_DEPTH], *b.depth_near, width, height, behind_keep};
}

/* Whether `p`, in lens space, lies behind what the picture drew at `at`. */
static bool
behind(const raster_lines_depth_t* depth, vec3f_t p, vec2f_t at) {
    const int pw = depth->drawn.width;
    const int ph = depth->drawn.height;
    const int x = (int)(at.x * (float)pw / (float)depth->width);
    const int y = (int)(at.y * (float)ph / (float)depth->height);
    if (x < 0 || y < 0 || x >= pw || y >= ph) {
        return false;
    }
    /* The farthest drawn depth around it: an edge lying on a surface seen at
     * a grazing angle stays in front, though its own pixel may be deeper. */
    uint16_t farthest = R3D_DEPTH_NEAREST;
    for (int sy = mathi_max(y - NEIGHBOURS, 0); sy <= mathi_min(y + NEIGHBOURS, ph - 1); sy++) {
        const uint16_t* row = image_row(&depth->drawn, sy);
        for (int sx = mathi_max(x - NEIGHBOURS, 0); sx <= mathi_min(x + NEIGHBOURS, pw - 1); sx++) {
            farthest = row[sx] < farthest ? row[sx] : farthest;
        }
    }
    if (farthest == R3D_DEPTH_EMPTY) {
        return false;
    }
    const float surface = depth->depth_near * (float)R3D_DEPTH_NEAREST / (float)farthest;
    return p.z > surface * (1.0F + BEHIND_SLACK);
}

/* How far along a lens-space segment from depth `za` to `zb` the point is
 * that shows `shown` of the way along its picture: 1/z is linear on screen. */
static float
lens_share(float za, float zb, float shown) {
    return shown * za / (((1.0F - shown) * zb) + (shown * za));
}

void
raster_lines_segment(const r3d_lens_t* lens, const raster_lines_depth_t* depth, vec3f_t a, vec3f_t b,
                     gfx_color_t color) {
    if (!projectf_clip_near(lens->project.near_z, &a, &b)) {
        return;
    }
    const vec2f_t from = projectf_screen(&lens->project, a);
    const vec2f_t to = projectf_screen(&lens->project, b);
    if (depth == NULL) {
        draw_line((int)from.x, (int)from.y, (int)to.x, (int)to.y, color);
        return;
    }
    const gfx_color_t dimmed = rgb565be_scale(color, depth->behind_keep);
    const vec2f_t span = {to.x - from.x, to.y - from.y};
    const float length = mathf_sqrt((span.x * span.x) + (span.y * span.y));
    /* Off-picture lengths can be huge; a stretch never needs to be finer than a pixel of the picture. */
    const float longest = (float)(depth->width + depth->height);
    const int steps = 1 + (int)(mathf_min(length, longest) / RASTER_LINES_STEP_PX);
    vec2f_t start = from;
    for (int i = 1; i <= steps; i++) {
        /* Even stretches of the picture; each tests the lens-space point its middle shows. */
        const vec2f_t end = vec2f_lerp(from, to, (float)i / (float)steps);
        const float shown = ((float)i - 0.5F) / (float)steps;
        const vec2f_t mid = vec2f_lerp(from, to, shown);
        const bool hidden = behind(depth, vec3f_lerp(a, b, lens_share(a.z, b.z, shown)), mid);
        if (!hidden || depth->behind_keep != 0) {
            draw_line((int)start.x, (int)start.y, (int)end.x, (int)end.y, hidden ? dimmed : color);
        }
        start = end;
    }
}

void
raster_lines_box(const r3d_lens_t* lens, const raster_lines_depth_t* depth, box3f_t box, gfx_color_t color) {
    vec3f_t corner[BOX_CORNERS];
    for (int k = 0; k < BOX_CORNERS; k++) {
        corner[k] = mat4f_apply(&lens->m, box3f_corner(box, k));
    }
    for (int a = 0; a < BOX_CORNERS; a++) {
        for (int bit = 1; bit < BOX_CORNERS; bit <<= 1) {
            if ((a & bit) == 0) {
                raster_lines_segment(lens, depth, corner[a], corner[a | bit], color);
            }
        }
    }
}
