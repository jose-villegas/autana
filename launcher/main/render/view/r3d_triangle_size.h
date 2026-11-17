/*
 * r3d_triangle_size: a triangle on screen by how many pixel centres it
 * covers under the top-left rule and its fill colour mode. Development
 * views and host tools share these bins.
 */
#pragma once

#include <stdbool.h>

#include "math/geometry/recti.h"

#define R3D_SIZES_SMALL_MAX 4

enum { R3D_SIZES_ZERO, R3D_SIZES_ONE, R3D_SIZES_TWO_TO_FOUR, R3D_SIZES_MORE, R3D_SIZES_BINS };

/* A face colour (r3d_span_triangle_solid), zero quantized colour steps, or
 * stepped colour from the draw's lit corners. */
enum { R3D_SIZES_FACE, R3D_SIZES_UNIFORM, R3D_SIZES_SHADED, R3D_SIZES_MODES };

/* X(name, mode, bin) for every mode and bin; optional leading arguments
 * let a containing field table pass its visitor through. */
#define R3D_SIZES_FIELDS(X, ...)                                                                                       \
    X(__VA_ARGS__ face_zero, R3D_SIZES_FACE, R3D_SIZES_ZERO)                                                           \
    X(__VA_ARGS__ face_one, R3D_SIZES_FACE, R3D_SIZES_ONE)                                                             \
    X(__VA_ARGS__ face_two_to_four, R3D_SIZES_FACE, R3D_SIZES_TWO_TO_FOUR)                                             \
    X(__VA_ARGS__ face_more, R3D_SIZES_FACE, R3D_SIZES_MORE)                                                           \
    X(__VA_ARGS__ uniform_zero, R3D_SIZES_UNIFORM, R3D_SIZES_ZERO)                                                     \
    X(__VA_ARGS__ uniform_one, R3D_SIZES_UNIFORM, R3D_SIZES_ONE)                                                       \
    X(__VA_ARGS__ uniform_two_to_four, R3D_SIZES_UNIFORM, R3D_SIZES_TWO_TO_FOUR)                                       \
    X(__VA_ARGS__ uniform_more, R3D_SIZES_UNIFORM, R3D_SIZES_MORE)                                                     \
    X(__VA_ARGS__ shaded_zero, R3D_SIZES_SHADED, R3D_SIZES_ZERO)                                                       \
    X(__VA_ARGS__ shaded_one, R3D_SIZES_SHADED, R3D_SIZES_ONE)                                                         \
    X(__VA_ARGS__ shaded_two_to_four, R3D_SIZES_SHADED, R3D_SIZES_TWO_TO_FOUR)                                         \
    X(__VA_ARGS__ shaded_more, R3D_SIZES_SHADED, R3D_SIZES_MORE)

/* A corner in pixels; pixel i's centre is at i + 0.5. */
typedef struct {
    float x, y;
} r3d_size_point_t;

/* Twice the signed area of a, b, c in pixels squared. */
float r3d_size_area2(r3d_size_point_t a, r3d_size_point_t b, r3d_size_point_t c);

/* The pixel centres inside the corners' bounding box. */
recti_t r3d_size_centre_box(const r3d_size_point_t v[3]);

/* The size bin of an exact covered-centre count. */
int r3d_size_bin_of(int covered);

/* The size bin of either winding, clipped to the screen. */
int r3d_size_bin(const r3d_size_point_t v[3], recti_t box, int width, int height);

/* Exact covered centres of either winding, clipped to the screen. */
int r3d_size_pixels(const r3d_size_point_t v[3], recti_t box, int width, int height);
