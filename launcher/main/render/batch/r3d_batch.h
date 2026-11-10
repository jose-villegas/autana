/* r3d_batch: copies of one mesh drawn from one packed table, not an instance
 * each (docs/assets/formats/SCNE.md#batches). The rows are SCNE's, read in
 * place; the cull tests each chunk's sphere, then each copy's, and draws the
 * copies it keeps as instances of their own. */
#pragma once

#include <stdint.h>

#include "math/geometry/spheref.h"
#include "math/linear/mat4f.h"
#include "render/r3d_instance.h"
#include "render/r3d_lit_mesh.h"

typedef struct r3d_lens r3d_lens_t; /* r3d_pipeline.h */

/* A batch's copies in runs of this many, each run a chunk with one sphere. */
#define R3D_BATCH_CHUNK_COPIES 16
/* A copy's offset from its chunk's centre and the three smaller components of
 * its rotation's unit quaternion, each over this many steps of its range. */
#define R3D_COPY_STEPS         32767
/* 1/sqrt(2): no smaller component of a unit quaternion is larger in magnitude. */
#define R3D_COPY_TURN_RANGE    0.70710678F

/* A chunk's frame: its copies' offsets count in reach / R3D_COPY_STEPS. */
typedef struct {
    vec3f_t centre; /* the batch's frame */
    float reach;    /* no copy's position is farther from `centre` */
} r3d_copy_chunk_t;

/* What the cull reads of every copy. */
typedef struct {
    int16_t offset[3];   /* from the chunk's centre */
    uint16_t colour_set; /* the mesh's LCOL set */
} r3d_copy_place_t;

/* A copy's rotation: a unit quaternion's three smaller components, each in
 * R3D_COPY_STEPS of R3D_COPY_TURN_RANGE, and which one the largest, never negative,
 * stood in for. Read only for copies the cull keeps. */
typedef struct {
    int16_t rotation[3];
    uint16_t largest; /* 0..3: x, y, z, w */
} r3d_copy_turn_t;

typedef struct {
    const r3d_copy_chunk_t* chunks; /* chunk_count, the batch's frame */
    const r3d_copy_place_t* places; /* copy_count */
    const r3d_copy_turn_t* turns;   /* copy_count */
    uint32_t* lod_coarse;           /* lod_words a copy, copy by copy, or NULL: the finest level */
    uint32_t colour_stride;         /* elements of one colour set; the mesh view holds set 0 */
    float copy_radius;              /* a copy's sphere about its position, whatever its turn: r3d_batch_copy_radius() */
    uint16_t copy_count, chunk_count, lod_words;
} r3d_batch_t;

/* A batch drawn: `instance` places every copy by its model and culls them
 * whole by its world box; its coarse bits are unread, each copy has its own
 * in the batch. Kept by the raster's cull as one copy, `instance` is that
 * copy's (its coarse bits, no world box) and `copy` names it. */
typedef struct {
    r3d_instance_t instance;
    const r3d_batch_t* batch;
    int copy;
} r3d_batch_draw_t;

/* The chunks a batch of `copies` holds. */
int r3d_batch_chunks(int copies);

/* Copy `copy`'s position, the batch's frame. */
vec3f_t r3d_batch_copy_position(const r3d_batch_t* batch, int copy);

/* Copy `copy`'s placement in the batch's frame: its rotation, then its position. */
mat4f_t r3d_batch_copy_model(const r3d_batch_t* batch, int copy);

/* How far from a copy's position its mesh reaches, in model units, however
 * it is turned: the distance to its root box's centre and that box's half
 * diagonal. */
float r3d_batch_copy_radius(const r3d_lit_mesh_t* mesh);

/* Chunk `chunk`'s sphere, the batch's frame: every copy's within it. */
spheref_t r3d_batch_chunk_sphere(const r3d_batch_t* batch, int chunk);

/* `mesh` with copy `copy`'s colour set in place of set 0, into `out`. */
void r3d_batch_copy_mesh(const r3d_batch_t* batch, const r3d_lit_mesh_t* mesh, int copy, r3d_lit_mesh_t* out);

/* r3d_lens_place() for kept copy `draw`: the batch's model, then the copy's
 * own placement. */
void r3d_batch_copy_place(const r3d_batch_draw_t* draw, r3d_lens_t* lens);
