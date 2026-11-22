/*
 * r3d_texture_internal: what r3d_texture.c shares with its tests: how the
 * colormap lights a colour and the distance under which a level draws flat.
 * Not a public module; r3d_texture.h is.
 */
#pragma once

#include <stdint.h>

/* The OKLab distance under which a colour is not told from another, the
 * just noticeable difference (CSS Color 4): a level whose colours all draw
 * this near the pattern's mean, at every light level, draws flat. */
#define R3D_TEXTURE_FLAT_JND 0.02F

/* The luminance the colormap divides out of `tint`: never 0. */
int r3d_texture_tint_luma(const uint8_t tint[3]);

/* A colormap channel, 0..255: the albedo `albedo_x_one` (the tint channel
 * times the pattern's, R3D_TEXTURE_ONE a pattern of 1.0) at light level
 * `level` of a tint of luminance `tint_luma`. */
int r3d_texture_light_channel(int albedo_x_one, int level, int tint_luma);
