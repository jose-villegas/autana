/*
 * stress_preset: the stress scene's corners. Each preset is one shape,
 * tessellated, shown in some number of copies laid out in front of the
 * camera, chosen so that one render option's cost shows: where it should
 * lose, beside a preset where it should win. Pure: what each copy is made of
 * and where it stands, from the preset and the camera's framing, so a host
 * test holds every preset to its corner. scene_stress.c shows them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/transformf.h"
#include "math/linear/vec3f.h"
#include "render/dynamic/r3d_dynamic_mesh.h"

typedef enum {
    STRESS_SHAPE_GRID,   /* primitive_grid(): a square facing the camera */
    STRESS_SHAPE_SPHERE, /* primitive_sphere(): closed, half of it facing away */
} stress_shape_t;

typedef enum {
    STRESS_LAYOUT_WALL,  /* side by side in one plane, apart: nothing covers anything */
    STRESS_LAYOUT_STACK, /* one behind another, each the arrangement's size: depth complexity = copies */
} stress_layout_t;

typedef struct {
    stress_shape_t shape;
    int cuts; /* the shape's tessellation: grid cells or sphere rings a side */
    int copies;
    stress_layout_t layout;
    int coverage_percent; /* how much of the picture's short side the arrangement spans */
    bool per_frame;       /* the mesh is made again every frame, a wave running over it */
} stress_params_t;

/* X(ID, key, per_frame, params...): the one place a preset is spelled
 * out, its key the scene key's suffix and its menu label. The scene table
 * and every test expand it; the render_lab tools README names the option
 * each is the loss case of. */
#define STRESS_PRESETS(X)                                                                                              \
    X(OPEN, "open", false, STRESS_SHAPE_GRID, 8, 16, STRESS_LAYOUT_WALL, 60)                                           \
    X(STACK, "stack", false, STRESS_SHAPE_GRID, 8, 8, STRESS_LAYOUT_STACK, 90)                                         \
    X(TINY_COPIES, "tiny-copies", false, STRESS_SHAPE_SPHERE, 4, 64, STRESS_LAYOUT_WALL, 40)                           \
    X(BIG_TRIS, "big-tris", false, STRESS_SHAPE_GRID, 2, 1, STRESS_LAYOUT_WALL, 100)                                   \
    X(PER_FRAME, "per-frame", true, STRESS_SHAPE_GRID, 24, 1, STRESS_LAYOUT_WALL, 80)

#define STRESS_PRESET_ID(id_, ...) STRESS_##id_,

typedef enum { STRESS_PRESETS(STRESS_PRESET_ID) STRESS_PRESET_COUNT } stress_preset_t;

extern const stress_params_t stress_presets[STRESS_PRESET_COUNT];

/* A copy's mesh: the shape's vertex and triangle counts. */
int stress_vertex_count(const stress_params_t* p);
int stress_triangle_count(const stress_params_t* p);

/* What stress_source() writes into: stress_vertex_count() positions and
 * colours, stress_triangle_count() triangles. */
typedef struct {
    vec3f_t* positions;
    uint8_t (*colors)[3];
    uint16_t (*triangles)[3];
} stress_buffers_t;

/* Positions are stored at this many ticks per unit: a unit shape and its
 * wave stay inside int16. */
#define STRESS_TICKS_PER_UNIT 16384

/* A copy's mesh `t_ms` into its wave, which only a per-frame preset reads:
 * the unit shape, banded in colour across its height, and for a per-frame
 * preset pushed out along its normal by the wave. False when the shape cannot be
 * made. Equal arguments give equal bytes. */
bool stress_source(const stress_params_t* p, uint32_t t_ms, const stress_buffers_t* b, r3d_dynamic_source_t* out);

/* Where the arrangement is seen from: the camera's distance from the
 * origin, which it looks at, and its lens's half field of view across the
 * picture's short side, as a tangent. */
typedef struct {
    float distance;
    float half_fov_short_tan;
} stress_framing_t;

/* Copy `copy`'s place in the arrangement's frame, whose +z faces the
 * camera: a WALL in the plane z = 0 about the origin, a STACK from z = 0
 * away from the camera, either spanning coverage_percent of what the camera
 * sees across the picture's short side at the origin. */
transformf_t stress_copy_place(const stress_params_t* p, int copy, stress_framing_t framing);

/* The longest side of a front copy's tessellation, in pixels of a picture
 * whose short side is `short_px`, seen square on: the size the preset's
 * triangles are drawn at. */
float stress_cut_px(const stress_params_t* p, int short_px);
