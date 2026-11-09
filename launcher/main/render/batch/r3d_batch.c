/* r3d_batch: decoding a batch's packed copies (docs/assets/formats/SCNE.md#batches). */
#include "render/batch/r3d_batch.h"

#include "math/linear/quatf.h"
#include "math/scalar/mathf.h"

int
r3d_batch_chunks(int copies) {
    return (copies + R3D_BATCH_CHUNK_COPIES - 1) / R3D_BATCH_CHUNK_COPIES;
}

vec3f_t
r3d_batch_copy_position(const r3d_batch_t* batch, int copy) {
    const r3d_copy_chunk_t* chunk = &batch->chunks[copy / R3D_BATCH_CHUNK_COPIES];
    const int16_t* offset = batch->places[copy].offset;
    const float step = chunk->reach / (float)R3D_COPY_STEPS;
    return vec3f_add(chunk->centre, vec3f_scale((vec3f_t){offset[0], offset[1], offset[2]}, step));
}

mat4f_t
r3d_batch_copy_model(const r3d_batch_t* batch, int copy) {
    const r3d_copy_turn_t* turn = &batch->turns[copy];
    const float step = R3D_COPY_TURN_RANGE / (float)R3D_COPY_STEPS;
    float q[4];
    float rest = 1.0F;
    for (int i = 0, k = 0; i < 4; i++) {
        if (i != turn->largest) {
            q[i] = (float)turn->rotation[k++] * step;
            rest -= q[i] * q[i];
        }
    }
    q[turn->largest] = mathf_sqrt(mathf_max(rest, 0.0F));
    return mat4f_from_trs(r3d_batch_copy_position(batch, copy), (quatf_t){q[0], q[1], q[2], q[3]}, vec3f_one());
}

float
r3d_batch_copy_radius(const r3d_lit_mesh_t* mesh) {
    const box3f_t root = box3f_from_box3s(mesh->nodes[0].bounds, 1.0F / (float)mesh->position_scale);
    const vec3f_t centre = box3f_centre(root);
    const vec3f_t half = box3f_half_extent(root);
    return mathf_sqrt(vec3f_dot(centre, centre)) + mathf_sqrt(vec3f_dot(half, half));
}

spheref_t
r3d_batch_chunk_sphere(const r3d_batch_t* batch, int chunk) {
    return (spheref_t){batch->chunks[chunk].centre, batch->chunks[chunk].reach + batch->copy_radius};
}

void
r3d_batch_copy_mesh(const r3d_batch_t* batch, const r3d_lit_mesh_t* mesh, int copy, r3d_lit_mesh_t* out) {
    *out = *mesh;
    const uint32_t at = (uint32_t)batch->places[copy].colour_set * batch->colour_stride;
    if (out->colors != NULL) {
        out->colors += at;
    }
    if (out->face_colors != NULL) {
        out->face_colors += at;
    }
}

void
r3d_batch_copy_place(const r3d_batch_draw_t* draw, r3d_lens_t* lens) {
    mat4f_t model = r3d_batch_copy_model(draw->batch, draw->copy);
    if (draw->instance.model != NULL) {
        model = mat4f_mul_affine(*draw->instance.model, model);
    }
    r3d_lens_place(lens, &model, draw->instance.mesh->position_scale);
}
