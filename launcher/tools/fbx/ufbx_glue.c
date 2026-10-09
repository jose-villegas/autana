/* Flattens an FBX scene, read by ufbx, into plain arrays for fbx_to_glb.py.
 * ufbx's structs are large and change between releases; this file is the only
 * place that knows them, and Python sees counts and float buffers. The scene
 * is read as glTF wants it: metres, right-handed, +Y up, pivots folded into
 * the node transforms, geometry transforms folded into the vertices.
 * Compiled together with ufbx.c by tools/fbx/ufbx_lib.py. */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "ufbx.c"

#ifdef _WIN32
#define FBXG_API __declspec(dllexport)
#else
#define FBXG_API
#endif

/* glTF stores at most four influences per vertex. */
#define FBXG_INFLUENCES 4

typedef struct fbxg_scene {
    ufbx_scene* scene;
    ufbx_baked_anim** baked; /* one per animation stack */
} fbxg_scene;

FBXG_API fbxg_scene*
fbxg_open(const char* path, char* error, size_t error_size) {
    ufbx_load_opts opts = {0};
    opts.target_axes = ufbx_axes_right_handed_y_up;
    opts.target_unit_meters = 1.0;
    opts.space_conversion = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
    opts.pivot_handling = UFBX_PIVOT_HANDLING_ADJUST_TO_ROTATION_PIVOT;
    opts.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;
    opts.generate_missing_normals = true;
    ufbx_error err;
    ufbx_scene* scene = ufbx_load_file(path, &opts, &err);
    if (!scene) {
        ufbx_format_error(error, error_size, &err);
        return NULL;
    }
    fbxg_scene* s = (fbxg_scene*)calloc(1, sizeof *s);
    s->scene = scene;
    s->baked = (ufbx_baked_anim**)calloc(scene->anim_stacks.count + 1, sizeof *s->baked);
    for (size_t i = 0; i < scene->anim_stacks.count; i++) {
        ufbx_bake_opts bake = {0};
        bake.trim_start_time = true;
        s->baked[i] = ufbx_bake_anim(scene, scene->anim_stacks.data[i]->anim, &bake, &err);
        if (!s->baked[i]) {
            ufbx_format_error(error, error_size, &err);
            for (size_t j = 0; j < i; j++) {
                ufbx_free_baked_anim(s->baked[j]);
            }
            free(s->baked);
            ufbx_free_scene(scene);
            free(s);
            return NULL;
        }
    }
    return s;
}

FBXG_API void
fbxg_close(fbxg_scene* s) {
    for (size_t i = 0; i < s->scene->anim_stacks.count; i++) {
        ufbx_free_baked_anim(s->baked[i]);
    }
    free(s->baked);
    ufbx_free_scene(s->scene);
    free(s);
}

/* Nodes: glTF node i is ufbx node i (the root is node 0). */

FBXG_API size_t
fbxg_node_count(fbxg_scene* s) {
    return s->scene->nodes.count;
}

FBXG_API const char*
fbxg_node_name(fbxg_scene* s, size_t i) {
    return s->scene->nodes.data[i]->name.data;
}

/* trs: translation xyz, rotation quaternion xyzw, scale xyz. Returns the
 * parent's index, or -1 for the root. */
FBXG_API int
fbxg_node_transform(fbxg_scene* s, size_t i, double* trs) {
    const ufbx_node* n = s->scene->nodes.data[i];
    const ufbx_transform* t = &n->local_transform;
    trs[0] = t->translation.x;
    trs[1] = t->translation.y;
    trs[2] = t->translation.z;
    trs[3] = t->rotation.x;
    trs[4] = t->rotation.y;
    trs[5] = t->rotation.z;
    trs[6] = t->rotation.w;
    trs[7] = t->scale.x;
    trs[8] = t->scale.y;
    trs[9] = t->scale.z;
    return n->parent ? (int)n->parent->typed_id : -1;
}

/* Materials. */

FBXG_API size_t
fbxg_material_count(fbxg_scene* s) {
    return s->scene->materials.count;
}

FBXG_API const char*
fbxg_material_name(fbxg_scene* s, size_t i) {
    return s->scene->materials.data[i]->name.data;
}

/* rgba: the base colour factor. */
FBXG_API void
fbxg_material_colour(fbxg_scene* s, size_t i, double* rgba) {
    const ufbx_material* m = s->scene->materials.data[i];
    const ufbx_material_map* map = m->pbr.base_color.has_value ? &m->pbr.base_color : &m->fbx.diffuse_color;
    rgba[0] = map->value_vec4.x;
    rgba[1] = map->value_vec4.y;
    rgba[2] = map->value_vec4.z;
    rgba[3] = map->value_vec4.w;
}

/* Meshes, one per node that holds one. */

static const ufbx_skin_deformer*
node_skin(fbxg_scene* s, size_t node) {
    const ufbx_mesh* m = s->scene->nodes.data[node]->mesh;
    return m && m->skin_deformers.count ? m->skin_deformers.data[0] : NULL;
}

/* Triangle count of the node's mesh (0 when it has none); the number of skin
 * joints and whether it has vertex colours come back through the pointers. */
FBXG_API size_t
fbxg_mesh_triangles(fbxg_scene* s, size_t node, int* joints, int* has_colour) {
    const ufbx_mesh* m = s->scene->nodes.data[node]->mesh;
    const ufbx_skin_deformer* skin = node_skin(s, node);
    *joints = skin ? (int)skin->clusters.count : 0;
    *has_colour = m && m->vertex_color.exists;
    return m ? m->num_triangles : 0;
}

/* Fills the triangulated corners (three per triangle). A skinned mesh is
 * written in world space at its bind pose, which is how glTF reads one.
 * position, normal: 3 floats per corner. colour: 4. joint: 4 uint16.
 * weight: 4, the strongest influences, normalised. triangle_material: one per
 * triangle, the scene's material index or -1. */
FBXG_API void
fbxg_mesh_corners(fbxg_scene* s, size_t node, float* position, float* normal, float* colour, uint16_t* joint,
                  float* weight, int* triangle_material) {
    const ufbx_node* n = s->scene->nodes.data[node];
    const ufbx_mesh* m = n->mesh;
    const ufbx_skin_deformer* skin = node_skin(s, node);
    ufbx_matrix normal_to_world = ufbx_matrix_for_normals(&n->geometry_to_world);
    uint32_t* tris = (uint32_t*)malloc(m->max_face_triangles * 3 * sizeof *tris);
    size_t at = 0, tri = 0;
    for (size_t f = 0; f < m->num_faces; f++) {
        ufbx_face face = m->faces.data[f];
        int material = -1;
        if (m->face_material.count) {
            const ufbx_material* mat = m->materials.data[m->face_material.data[f]];
            if (mat) {
                material = (int)mat->typed_id;
            }
        }
        uint32_t count = ufbx_triangulate_face(tris, m->max_face_triangles * 3, m, face);
        for (uint32_t k = 0; k < count * 3; k++, at++) {
            uint32_t c = tris[k];
            ufbx_vec3 p = ufbx_get_vertex_vec3(&m->vertex_position, c);
            ufbx_vec3 nr = ufbx_get_vertex_vec3(&m->vertex_normal, c);
            if (skin) {
                p = ufbx_transform_position(&n->geometry_to_world, p);
                nr = ufbx_transform_direction(&normal_to_world, nr);
                double len = sqrt(nr.x * nr.x + nr.y * nr.y + nr.z * nr.z);
                if (len > 0.0) {
                    nr.x /= len;
                    nr.y /= len;
                    nr.z /= len;
                }
            }
            position[3 * at] = (float)p.x;
            position[3 * at + 1] = (float)p.y;
            position[3 * at + 2] = (float)p.z;
            normal[3 * at] = (float)nr.x;
            normal[3 * at + 1] = (float)nr.y;
            normal[3 * at + 2] = (float)nr.z;
            if (m->vertex_color.exists) {
                ufbx_vec4 col = ufbx_get_vertex_vec4(&m->vertex_color, c);
                colour[4 * at] = (float)col.x;
                colour[4 * at + 1] = (float)col.y;
                colour[4 * at + 2] = (float)col.z;
                colour[4 * at + 3] = (float)col.w;
            }
            if (skin) {
                ufbx_skin_vertex sv = skin->vertices.data[m->vertex_indices.data[c]];
                uint32_t used = sv.num_weights < FBXG_INFLUENCES ? sv.num_weights : FBXG_INFLUENCES;
                double total = 0.0;
                for (uint32_t w = 0; w < FBXG_INFLUENCES; w++) {
                    joint[FBXG_INFLUENCES * at + w] = 0;
                    weight[FBXG_INFLUENCES * at + w] = 0.0f;
                }
                for (uint32_t w = 0; w < used; w++) {
                    total += skin->weights.data[sv.weight_begin + w].weight;
                }
                for (uint32_t w = 0; w < used && total > 0.0; w++) {
                    ufbx_skin_weight sw = skin->weights.data[sv.weight_begin + w];
                    joint[FBXG_INFLUENCES * at + w] = (uint16_t)sw.cluster_index;
                    weight[FBXG_INFLUENCES * at + w] = (float)(sw.weight / total);
                }
            }
        }
        for (uint32_t t = 0; t < count; t++) {
            triangle_material[tri++] = material;
        }
    }
    free(tris);
}

/* Joint j of the node's skin: the glTF node it moves, and its inverse bind
 * matrix, column-major as glTF stores it. */
FBXG_API int
fbxg_skin_joint(fbxg_scene* s, size_t node, size_t j, float* inverse_bind) {
    const ufbx_skin_cluster* c = node_skin(s, node)->clusters.data[j];
    ufbx_matrix inv = ufbx_matrix_invert(&c->bind_to_world);
    for (int col = 0; col < 4; col++) {
        inverse_bind[4 * col] = (float)inv.cols[col].x;
        inverse_bind[4 * col + 1] = (float)inv.cols[col].y;
        inverse_bind[4 * col + 2] = (float)inv.cols[col].z;
        inverse_bind[4 * col + 3] = col == 3 ? 1.0f : 0.0f;
    }
    return (int)c->bone_node->typed_id;
}

/* Animations: one per stack, baked to translation, rotation and scale keys
 * on each node it moves. */

FBXG_API size_t
fbxg_animation_count(fbxg_scene* s) {
    return s->scene->anim_stacks.count;
}

FBXG_API const char*
fbxg_animation_name(fbxg_scene* s, size_t i) {
    return s->scene->anim_stacks.data[i]->name.data;
}

FBXG_API size_t
fbxg_animation_nodes(fbxg_scene* s, size_t i) {
    return s->baked[i]->nodes.count;
}

/* Node j of animation i: returns the glTF node; counts gets its key counts
 * for translation, rotation and scale. */
FBXG_API int
fbxg_animation_node(fbxg_scene* s, size_t i, size_t j, size_t* counts) {
    const ufbx_baked_node* b = &s->baked[i]->nodes.data[j];
    counts[0] = b->translation_keys.count;
    counts[1] = b->rotation_keys.count;
    counts[2] = b->scale_keys.count;
    return (int)b->typed_id;
}

/* The keys of one channel (0 translation, 1 rotation, 2 scale): seconds, and
 * 3 or 4 floats each. */
FBXG_API void
fbxg_animation_keys(fbxg_scene* s, size_t i, size_t j, int channel, double* time, float* value) {
    const ufbx_baked_node* b = &s->baked[i]->nodes.data[j];
    if (channel == 1) {
        for (size_t k = 0; k < b->rotation_keys.count; k++) {
            const ufbx_baked_quat* key = &b->rotation_keys.data[k];
            time[k] = key->time;
            value[4 * k] = (float)key->value.x;
            value[4 * k + 1] = (float)key->value.y;
            value[4 * k + 2] = (float)key->value.z;
            value[4 * k + 3] = (float)key->value.w;
        }
        return;
    }
    const ufbx_baked_vec3_list* keys = channel == 0 ? &b->translation_keys : &b->scale_keys;
    for (size_t k = 0; k < keys->count; k++) {
        time[k] = keys->data[k].time;
        value[3 * k] = (float)keys->data[k].value.x;
        value[3 * k + 1] = (float)keys->data[k].value.y;
        value[3 * k + 2] = (float)keys->data[k].value.z;
    }
}
