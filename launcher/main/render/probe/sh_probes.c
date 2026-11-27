/* sh_probes: each grid coefficient is interpolated before cosine convolution. */
#include "render/probe/sh_probes.h"
#include <assert.h>
#include <math.h>
#include <stddef.h>
#include <string.h>
#include "math/scalar/mathf.h"

bool
sh_probes_inside(const sh_probes_t* probes, int x, int y, int z) {
    assert(x >= 0 && x < probes->dims[0]);
    assert(y >= 0 && y < probes->dims[1]);
    assert(z >= 0 && z < probes->dims[2]);
    const size_t index = ((((size_t)z * probes->dims[1]) + (size_t)y) * probes->dims[0]) + (size_t)x;
    return probes->inside != NULL
           && (probes->inside[index / SH_PROBES_MASK_BITS_PER_BYTE] & (1U << (index % SH_PROBES_MASK_BITS_PER_BYTE)))
                  != 0;
}

sh1_rgb_t
sh_probes_sample(const sh_probes_t* probes, vec3f_t world) {
    const float position[] = {world.x, world.y, world.z};
    const float origin[] = {probes->origin.x, probes->origin.y, probes->origin.z};
    const float spacing[] = {probes->spacing.x, probes->spacing.y, probes->spacing.z};
    int indices[MAT3F_AXES][2];
    float weight[MAT3F_AXES];
    for (int axis = 0; axis < MAT3F_AXES; axis++) {
        assert(probes->dims[axis] > 0);
        assert(probes->dims[axis] == 1 || spacing[axis] > 0);
        const float at = probes->dims[axis] == 1 ? 0.0F
                                                 : mathf_clamp((position[axis] - origin[axis]) / spacing[axis], 0.0F,
                                                               (float)(probes->dims[axis] - 1));
        indices[axis][0] = (int)at;
        indices[axis][1] = indices[axis][0] + (indices[axis][0] < probes->dims[axis] - 1);
        weight[axis] = at - (float)indices[axis][0];
    }
    sh1_rgb_t rows[2][2];
    for (int z = 0; z < 2; z++) {
        for (int y = 0; y < 2; y++) {
            sh1_rgb_t ends[2];
            for (int x = 0; x < 2; x++) {
                const size_t index = ((size_t)indices[2][z] * probes->dims[1] * probes->dims[0])
                                     + ((size_t)indices[1][y] * probes->dims[0]) + (size_t)indices[0][x];
                const int16_t* packed = probes->coefficients[index];
                for (int c = 0; c < SH1_COEFFICIENTS; c++) {
                    ends[x].c[c] = (vec3f_t){(float)packed[c * SH1_CHANNELS] * probes->scale,
                                             (float)packed[(c * SH1_CHANNELS) + 1] * probes->scale,
                                             (float)packed[(c * SH1_CHANNELS) + 2] * probes->scale};
                }
            }
            rows[z][y] = sh1_rgb_lerp(ends[0], ends[1], weight[0]);
        }
    }
    return sh1_rgb_lerp(sh1_rgb_lerp(rows[0][0], rows[0][1], weight[1]),
                        sh1_rgb_lerp(rows[1][0], rows[1][1], weight[1]), weight[2]);
}

_Static_assert(SH_PROBES_HEADER_SIZE % _Alignof(int16_t) == 0, "SHPR coefficient alignment");

/* The probe count of a finite grid with positive spacing whose records fit `payload`. */
static asset_status_t
grid_count(const sh_probes_t* grid, size_t payload, size_t* count) {
    const float origin[] = {grid->origin.x, grid->origin.y, grid->origin.z};
    const float spacing[] = {grid->spacing.x, grid->spacing.y, grid->spacing.z};
    *count = 1;
    for (int axis = 0; axis < MAT3F_AXES; axis++) {
        if (grid->dims[axis] == 0 || !isfinite(origin[axis]) || !isfinite(spacing[axis]) || spacing[axis] <= 0) {
            return ASSET_ERR_FORMAT;
        }
        if (*count > payload / SH_PROBES_RECORD_SIZE / grid->dims[axis]) {
            return ASSET_ERR_SIZE;
        }
        *count *= grid->dims[axis];
    }
    return ASSET_OK;
}

/* The inside mask's bits past the last probe are zero. */
static bool
mask_tail_zero(const uint8_t* inside, size_t mask_size, size_t count) {
    const unsigned used = count % SH_PROBES_MASK_BITS_PER_BYTE;
    return used == 0 || (inside[mask_size - 1] >> used) == 0;
}

asset_status_t
sh_probes_read(asset_view_t entry, sh_probes_t* out) {
    *out = (sh_probes_t){0};
    asset_status_t status =
        asset_entry_validate(entry, SH_PROBES_VERSION, SH_PROBES_AT_VERSION, SH_PROBES_HEADER_SIZE, _Alignof(int16_t));
    if (status != ASSET_OK) {
        return status;
    }
    uint16_t flags;
    memcpy(&flags, entry.data + SH_PROBES_AT_FLAGS, sizeof flags);
    if ((flags & ~SH_PROBES_FLAG_INSIDE_MASK) != 0
        || !asset_entry_zero(entry, SH_PROBES_AT_PADDING, SH_PROBES_HEADER_SIZE)) {
        return ASSET_ERR_FORMAT;
    }
    sh_probes_t grid = {0};
    memcpy(&grid.origin, entry.data + SH_PROBES_AT_ORIGIN, sizeof grid.origin);
    memcpy(&grid.spacing, entry.data + SH_PROBES_AT_SPACING, sizeof grid.spacing);
    memcpy(grid.dims, entry.data + SH_PROBES_AT_DIMS, sizeof grid.dims);
    memcpy(&grid.scale, entry.data + SH_PROBES_AT_SCALE, sizeof grid.scale);
    const size_t payload = entry.size - SH_PROBES_HEADER_SIZE;
    size_t count;
    status = grid_count(&grid, payload, &count);
    if (status != ASSET_OK) {
        return status;
    }
    const size_t coefficients_size = count * SH_PROBES_RECORD_SIZE;
    const size_t mask_size = (flags & SH_PROBES_FLAG_INSIDE_MASK) != 0
                                 ? (count + SH_PROBES_MASK_BITS_PER_BYTE - 1) / SH_PROBES_MASK_BITS_PER_BYTE
                                 : 0;
    if (payload - coefficients_size != mask_size) {
        return ASSET_ERR_SIZE;
    }
    if (!isfinite(grid.scale) || grid.scale <= 0) {
        return ASSET_ERR_FORMAT;
    }
    grid.coefficients = (const int16_t (*)[SH_PROBES_COMPONENTS])(entry.data + SH_PROBES_HEADER_SIZE);
    grid.inside = mask_size != 0 ? entry.data + SH_PROBES_HEADER_SIZE + coefficients_size : NULL;
    if (mask_size != 0 && !mask_tail_zero(grid.inside, mask_size, count)) {
        return ASSET_ERR_FORMAT;
    }
    *out = grid;
    return ASSET_OK;
}
