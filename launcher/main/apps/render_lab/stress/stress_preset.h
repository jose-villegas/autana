/* stress_preset: deterministic copy layouts and generated surfaces for renderer workloads. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "math/linear/transformf.h"
#include "math/linear/vec3f.h"
#include "render/dynamic/r3d_dynamic_mesh.h"

typedef enum {
    STRESS_SHAPE_PLANE,    /* primitive_plane(): a square facing the camera */
    STRESS_SHAPE_SPHERE,   /* primitive_sphere(): closed, half of it facing away */
    STRESS_SHAPE_CUBE,     /* primitive_cube(): closed, its back face facing away */
    STRESS_SHAPE_CYLINDER, /* primitive_cylinder(): its axis toward the camera, an end's fan of slivers in view */
    STRESS_SHAPE_GRID,     /* independent triangle corners on a tessellated square */
    STRESS_SHAPE_CARD,     /* perforated square with uneven tessellation */
} stress_shape_t;

typedef enum {
    STRESS_SURFACE_BANDED,    /* still colour bands up the copy, each triangle's corners close */
    STRESS_SURFACE_WAVE,      /* the bands run up it, pushing it out: the mesh made again every frame */
    STRESS_SURFACE_NOISY,     /* neighbouring vertices at opposite colours: every triangle's corners far apart */
    STRESS_SURFACE_MIXED,     /* alternating uniform and stepped triangle colours */
    STRESS_SURFACE_CARD,      /* procedural texture sampled at each corner */
    STRESS_SURFACE_UNIFORM,   /* one colour at every corner: zero colour steps, filled uniform */
    STRESS_SURFACE_STATIC,    /* the pack's own static mesh asset per copy, nothing generated */
    STRESS_SURFACE_TWO_SIDED, /* banded, and drawn from both sides: nothing faces away */
} stress_surface_t;

typedef enum {
    STRESS_LAYOUT_WALL,  /* side by side in one plane, apart: nothing covers anything */
    STRESS_LAYOUT_STACK, /* one behind another, each the arrangement's size: depth complexity = copies */
} stress_layout_t;

#define STRESS_SCENE         "stress"
#define STRESS_CAMERA        "camera"
#define STRESS_ORBIT_CLIP    "orbit"
#define STRESS_CAMERA_CULLED "culled"
#define STRESS_CAMERA_NEAR   "near"

typedef struct {
    stress_shape_t shape;
    int cuts; /* the shape's tessellation: subdivisions, sphere rings or cylinder segments */
    int copies;
    stress_layout_t layout;
    int coverage_percent; /* how much of the picture's short side the arrangement spans */
    stress_surface_t surface;
    const char* camera;
    int perf_poses; /* poses the stress perf suite replays along the swing */
} stress_params_t;

/* X(ID, key, surface): each preset, its key the scene key's suffix and its
 * menu label. stress_presets gives each its shape and layout, one row per
 * ID. The scene table and every test expand it; the render_lab tools README
 * names the option each is the loss case of. */
#define STRESS_PRESETS(X)                                                                                              \
    X(OPEN, "open", BANDED)                                                                                            \
    X(STACK, "stack", BANDED)                                                                                          \
    X(TINY_COPIES, "tiny-copies", BANDED)                                                                              \
    X(BIG_TRIS, "big-tris", BANDED)                                                                                    \
    X(PER_FRAME, "per-frame", WAVE)                                                                                    \
    X(TWO_SIDED, "two-sided", TWO_SIDED)                                                                               \
    X(NOISY_TINY, "noisy-tiny", NOISY)                                                                                 \
    X(MIX, "mix", MIXED)                                                                                               \
    X(STATIC_COPIES, "static-copies", STATIC)                                                                          \
    X(UNIFORM_MID, "uniform-mid", UNIFORM)                                                                             \
    X(UNIFORM_FULL, "uniform-full", UNIFORM)                                                                           \
    X(CARD, "card", CARD)                                                                                              \
    X(CULLED, "culled", BANDED)                                                                                        \
    X(NEAR, "near", BANDED)

#define STRESS_PRESET_ID(id_, ...) STRESS_##id_,

typedef enum { STRESS_PRESETS(STRESS_PRESET_ID) STRESS_PRESET_COUNT } stress_preset_t;

extern const stress_params_t stress_presets[STRESS_PRESET_COUNT];

/* Whether a surface's mesh is made again every frame: a constant
 * expression, so a static table can read it. */
#define STRESS_SURFACE_IS_PER_FRAME(surface_) ((surface_) == STRESS_SURFACE_WAVE)

static inline bool
stress_per_frame(const stress_params_t* p) {
    return STRESS_SURFACE_IS_PER_FRAME(p->surface);
}

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
 * the unit shape coloured as its surface says, and for a per-frame preset
 * pushed out along its normal by the wave. False when the shape cannot be
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

/* Mixed copies cycle through tessellations; half their triangles have equal corner colours. */
#define STRESS_MIX_LEVELS         4
#define STRESS_MIX_UNIFORM_PERIOD 2
stress_params_t stress_copy_params(const stress_params_t* p, int copy);
