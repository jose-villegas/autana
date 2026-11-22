#include "render/view/raster_probes.h"

#include <limits.h>
#include <math.h>
#include "core/tune.h"
#include "draw/draw.h"
#include "math/scalar/mathf.h"
#include "render/view/raster_lines.h"

#define OUTPUT_RB_SHIFT (CHAR_BIT - RGB565_GREEN_AT)
#define OUTPUT_G_SHIFT  (CHAR_BIT - (RGB565_RED_AT - RGB565_GREEN_AT))

TUNE_OWNER(probeview);
TUNE(probeview, size, RASTER_PROBES_SIZE_DEFAULT, 1, RASTER_PROBES_SIZE_MAX);

static void
begin(const raster_attachment_t* self, const raster_t* raster, const render_view_t* view) {
    raster_probes_t* s = self->state;
    s->raster = raster;
    s->camera = *view;
    s->probes = raster->probes;
}

raster_attachment_t
raster_probes_view(void* state) {
    return (raster_attachment_t){.begin = begin, .state = state};
}

static gfx_color_t
shade(const sh1_irradiance_t* ambient, vec3f_t normal, const r3d_skin_light_transfer_t* transfer) {
    const vec3f_t light = sh1_irradiance_eval(ambient, normal);
    return rgb565be_pack(r3d_skin_light_encode(light.x, transfer) >> OUTPUT_RB_SHIFT,
                         r3d_skin_light_encode(light.y, transfer) >> OUTPUT_G_SHIFT,
                         r3d_skin_light_encode(light.z, transfer) >> OUTPUT_RB_SHIFT);
}

static void
cross(const r3d_lens_t* lens, const raster_lines_depth_t* depth, vec3f_t centre) {
    const float arm = RASTER_PROBES_CROSS_RADIUS_PX * centre.z;
    raster_lines_segment(lens, depth, vec3f_add(centre, (vec3f_t){-arm, 0.0F, 0.0F}),
                         vec3f_add(centre, (vec3f_t){arm, 0.0F, 0.0F}), RASTER_PROBES_INSIDE_COLOR);
    raster_lines_segment(lens, depth, vec3f_add(centre, (vec3f_t){0.0F, -arm, 0.0F}),
                         vec3f_add(centre, (vec3f_t){0.0F, arm, 0.0F}), RASTER_PROBES_INSIDE_COLOR);
}

static void
disc(const r3d_skin_light_transfer_t* transfer, const render_view_t* seen, const raster_lines_depth_t* depth,
     vec3f_t centre, vec2f_t at, float radius, const sh1_rgb_t* radiance) {
    const float pixels = seen->lens.fx * radius / centre.z;
    const int left = (int)fmaxf(ceilf(at.x - pixels), 0.0F);
    const int right = (int)fminf(floorf(at.x + pixels), (float)(depth->width - 1));
    const int top = (int)fmaxf(ceilf(at.y - pixels), 0.0F);
    const int bottom = (int)fminf(floorf(at.y + pixels), (float)(depth->height - 1));
    const sh1_irradiance_t ambient = r3d_skin_light_ambient_sh(radiance);
    for (int y = top; y <= bottom; y++) {
        for (int x = left; x <= right; x++) {
            const float nx = ((float)x - at.x) / pixels;
            const float ny = ((float)y - at.y) / pixels;
            const float squared = (nx * nx) + (ny * ny);
            if (squared > 1.0F) {
                continue;
            }
            const float nz = mathf_sqrt(1.0F - squared);
            const vec3f_t surface = {0.0F, 0.0F, centre.z - (radius * nz)};
            if (surface.z <= seen->lens.near_z || raster_lines_hidden(depth, surface, (vec2f_t){(float)x, (float)y})) {
                continue;
            }
            const vec3f_t normal =
                vec3f_sub(vec3f_add(vec3f_scale(seen->screen_x, nx), vec3f_scale(seen->screen_y, ny)),
                          vec3f_scale(seen->forward, nz));
            draw_pixel(x, y, shade(&ambient, normal, transfer));
        }
    }
}

static void
probe(const sh_probes_t* probes, const r3d_skin_light_transfer_t* transfer, const render_view_t* seen,
      const r3d_lens_t* lens, const raster_lines_depth_t* depth, float radius, int x, int y, int z) {
    const vec3f_t world =
        vec3f_add(probes->origin,
                  (vec3f_t){(float)x * probes->spacing.x, (float)y * probes->spacing.y, (float)z * probes->spacing.z});
    const vec3f_t centre = mat4f_apply(&lens->m, world);
    if (centre.z <= seen->lens.near_z) {
        return;
    }
    const vec2f_t at = projectf_screen(&lens->project, centre);
    if (at.x < 0.0F || at.y < 0.0F || at.x >= (float)depth->width || at.y >= (float)depth->height) {
        return;
    }
    if (sh_probes_inside(probes, x, y, z)) {
        cross(lens, depth, centre);
    } else {
        const sh1_rgb_t radiance = sh_probes_sample(probes, world);
        disc(transfer, seen, depth, centre, at, radius, &radiance);
    }
}

void
raster_probes_overlay(void* state, int width, int height) {
    raster_probes_t* s = state;
    if (s->raster == NULL || s->probes == NULL) {
        return;
    }
    const sh_probes_t* probes = s->probes;
    const float spacing = mathf_min(probes->spacing.x, mathf_min(probes->spacing.y, probes->spacing.z));
    const float radius = spacing * (float)size / RASTER_PROBES_SIZE_UNIT;
    if (!(radius > 0.0F)) {
        return;
    }
    const r3d_skin_light_transfer_t* transfer = s->raster->light_transfer;
    if (transfer == NULL) {
        const float white = s->raster->tonemap_white;
        if (!s->fallback_ready || s->fallback_white != white) {
            r3d_skin_light_transfer_build(white, &s->fallback_transfer);
            s->fallback_white = white;
            s->fallback_ready = true;
        }
        transfer = &s->fallback_transfer;
    }
    render_view_t seen = s->camera;
    render_view_refit(&seen, (viewport_t){width, height, seen.viewport.quarter});
    r3d_lens_t lens;
    r3d_lens_init(&lens, &seen, 1);
    raster_lines_depth_t depth;
    raster_lines_depth(&depth, s->raster, width, height, 0);
    for (int z = 0; z < probes->dims[2]; z++) {
        for (int y = 0; y < probes->dims[1]; y++) {
            for (int x = 0; x < probes->dims[0]; x++) {
                probe(probes, transfer, &seen, &lens, &depth, radius, x, y, z);
            }
        }
    }
}
