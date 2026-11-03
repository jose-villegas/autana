/*
 * primitive_mesh: the standard shapes as indexed triangle meshes, for
 * geometry made at run time or baked: a plane, a sphere, a cube, a capsule
 * and a cylinder, each about the origin and each from a struct of its
 * named parameters. Each triangle winds counter-clockwise about its outward
 * normal in a right-handed frame, as iso_surface.h winds its own:
 * (b - a) x (c - a) points out of the front.
 *
 * The round shapes turn about +y in `segments` slices. The seam's vertices,
 * and a pole's or an apex's, repeat once per segment, so every vertex has
 * one place round the axis. A ring of radius 0 makes one triangle per
 * segment where another ring makes two; no triangle has zero area unless a
 * parameter the shape allows (a capsule's height of 0) makes it so.
 *
 * The caller owns the buffers; a shape that does not fit them, has
 * parameters out of range, or needs more vertices than a uint16 index
 * reaches, writes nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/vec3f.h"

typedef struct {
    vec3f_t* positions;
    int vertex_capacity;
    uint16_t (*triangles)[3];
    int triangle_capacity;
    int vertex_count, triangle_count;
} primitive_mesh_t;

/* The fewest slices round the axis that enclose a volume. */
#define PRIMITIVE_SEGMENTS_MIN     3
/* The fewest bands a sphere has: one cap of triangles round each pole. */
#define PRIMITIVE_SPHERE_RINGS_MIN 2

/* The square from (-0.5, -0.5) to (0.5, 0.5) at z = 0, facing +z, in
 * `subdivisions` x `subdivisions` squares, each cut in two. A quad is one
 * subdivision. */
typedef struct {
    int subdivisions;
} primitive_plane_t;

/* The sphere of radius 1, `rings` bands from +y to -y. */
typedef struct {
    int segments;
    int rings;
} primitive_sphere_t;

/* The cube of side 1, each face a plane of `subdivisions`, its own
 * vertices on its own edges. */
typedef struct {
    int subdivisions;
} primitive_cube_t;

/* A cylinder of `radius` and `height` between two hemispheres of `radius`,
 * each in `rings` bands, its axis on y: `height` + 2 `radius` tall. A
 * height of 0 leaves a sphere with a band of zero area at its equator. */
typedef struct {
    float radius;
    float height;
    int segments;
    int rings;
} primitive_capsule_t;

/* `height` tall on y, `top_radius` at +height/2 and `bottom_radius` at
 * -height/2, its side in `stacks` bands and each end of radius above 0
 * closed by a fan of `segments` triangles. One radius of 0 is a cone. */
typedef struct {
    float top_radius;
    float bottom_radius;
    float height;
    int segments;
    int stacks;
} primitive_cylinder_t;

/* What each shape makes: its vertex and triangle counts, as many as its
 * call writes. Parameters out of range give counts the call refuses. */
int primitive_plane_vertex_count(const primitive_plane_t* shape);
int primitive_plane_triangle_count(const primitive_plane_t* shape);
int primitive_sphere_vertex_count(const primitive_sphere_t* shape);
int primitive_sphere_triangle_count(const primitive_sphere_t* shape);
int primitive_cube_vertex_count(const primitive_cube_t* shape);
int primitive_cube_triangle_count(const primitive_cube_t* shape);
int primitive_capsule_vertex_count(const primitive_capsule_t* shape);
int primitive_capsule_triangle_count(const primitive_capsule_t* shape);
int primitive_cylinder_vertex_count(const primitive_cylinder_t* shape);
int primitive_cylinder_triangle_count(const primitive_cylinder_t* shape);

/* Each writes its shape into `out` from index 0. False, writing nothing,
 * for parameters out of range (counts under 1, segments under
 * PRIMITIVE_SEGMENTS_MIN, sphere rings under PRIMITIVE_SPHERE_RINGS_MIN, a
 * negative size, a cylinder with neither radius above 0) or a shape the
 * buffers or uint16 indices cannot hold. */
bool primitive_plane(const primitive_plane_t* shape, primitive_mesh_t* out);
bool primitive_sphere(const primitive_sphere_t* shape, primitive_mesh_t* out);
bool primitive_cube(const primitive_cube_t* shape, primitive_mesh_t* out);
bool primitive_capsule(const primitive_capsule_t* shape, primitive_mesh_t* out);
bool primitive_cylinder(const primitive_cylinder_t* shape, primitive_mesh_t* out);
