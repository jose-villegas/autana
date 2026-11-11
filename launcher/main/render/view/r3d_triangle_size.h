/*
 * r3d_triangle_size: a triangle on screen by how many pixel centres it
 * covers, counted by the top-left rule from its corners, so any version of
 * the pipeline means the same; and by how its colour is filled. The counted
 * pipeline (r3d_pipeline_count.h) bins every triangle it sets up this way,
 * and the triangle_sizes tool bins a mesh's from a pose. Development builds
 * only, like every render/view/ file.
 */
#pragma once

#include <stdbool.h>

#include "math/geometry/recti.h"

enum { R3D_SIZES_ZERO, R3D_SIZES_ONE, R3D_SIZES_TWO_TO_FOUR, R3D_SIZES_MORE, R3D_SIZES_BINS };

/* A face colour (r3d_span_triangle_solid), three equal corner colours, so
 * no colour step across it, or corners that differ. */
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

/* The size bin of a triangle winding positive (r3d_size_area2() > 0) whose
 * centre box `box` touches a `width` x `height` screen. */
int r3d_size_bin(const r3d_size_point_t v[3], recti_t box, int width, int height);
