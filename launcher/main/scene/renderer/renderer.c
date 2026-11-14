/* renderer: an entity's mesh, and the skin that deforms it. */
#include "scene/renderer/renderer.h"

#include <string.h>

#include "core/memory.h"
#include "render/texture/r3d_texture.h"
#include "scene/scene_internal.h"

/* The index of the entity's renderer in its scene `*scene`, or -1. */
static int
renderer_of(entity_t entity, scene_t** scene) {
    scene_row_t row;
    *scene = scene_entity_row(entity, &row);
    return *scene == NULL ? -1 : scene_renderer_index(*scene, row);
}

const char*
renderer_mesh_id(entity_t entity) {
    scene_t* scene;
    const int i = renderer_of(entity, &scene);
    if (i < 0) {
        return NULL;
    }
    return i < scene->asset.renderer_count ? scene->asset.renderers[i].mesh
                                           : scene->asset.skinned_renderers[i - scene->asset.renderer_count].mesh;
}

const r3d_lit_mesh_t*
renderer_mesh(entity_t entity) {
    scene_t* scene;
    const int i = renderer_of(entity, &scene);
    if (i < 0) {
        return NULL;
    }
    scene_skins_settle(scene);
    return &scene->renderers[i].mesh;
}

bool
renderer_set_mesh(entity_t entity, const r3d_lit_mesh_t* mesh) {
    scene_t* scene;
    const int i = renderer_of(entity, &scene);
    if (i < 0 || i >= scene->asset.renderer_count) {
        return false;
    }
    scene_renderer_t* r = &scene->renderers[i];
    /* A batch's colours, places and spheres are its mesh's. */
    if (r->batch != NULL || (uint32_t)R3D_LOD_STATE_WORDS(mesh->lod_group_count) > r->lod_words) {
        return false;
    }
    r->mesh = *mesh;
    /* The textures opened were the pack mesh's: another mesh opens its own,
     * checked against it, when textures are next asked for. */
    scene_renderer_texturing_free(r);
    scene_renderer_bound(r);
    /* Another mesh's groups are not these bits': start it at its finest. */
    if (r->lod_coarse != NULL) {
        memset(r->lod_coarse, 0, sizeof(uint32_t) * R3D_LOD_STATE_WORDS(mesh->lod_group_count));
    }
    return true;
}

static asset_status_t
open_renderer_skin(scene_t* scene, scene_row_t entity, int renderer, const char* id, scene_skinned_renderer_t* next) {
    asset_status_t status = r3d_skin_open(scene->pack, id, &next->skin);
    const scene_animator_t* animator = &scene->animators[entity];
    const scene_skinned_renderer_t* old = &scene->skinned_renderers[entity];
    next->bind = old->bound ? old->bind : scene->renderers[renderer].mesh;
    if (status == ASSET_OK
        && (next->skin.joint_count != animator->skeleton.joint_count
            || next->skin.vertex_count != (uint32_t)next->bind.vertex_count)) {
        status = ASSET_ERR_FORMAT;
    }
    next->light_mode = (skin_light_mode_t)next->skin.lighting_mode;
    next->bound = true;
    return status;
}

/* A lit scene's first skin takes the light transfer; with `take`, an ACTIVE
 * row takes its buffers too. False, with nothing taken, when they do not fit. */
static bool
take_skin_memory(scene_t* scene, scene_row_t row, scene_skinned_renderer_t* next, bool lit, bool take) {
    r3d_skin_light_transfer_t* transfer = NULL;
    if (lit && scene->light_transfer == NULL) {
        transfer = scene_light_transfer_take(scene);
        if (transfer == NULL) {
            return false;
        }
        scene->light_transfer = transfer;
    }
    if (take && (scene->flags[row] & SCENE_FLAG_ACTIVE) != 0 && !scene_skin_take(scene, row, next)) {
        if (transfer != NULL) {
            memory_free(transfer);
            scene->light_transfer = NULL;
        }
        return false;
    }
    return true;
}

/* Replaces the row's skin with `next`, giving the old one's buffers back. */
static void
install_skin(scene_t* scene, scene_row_t row, const scene_skinned_renderer_t* next) {
    scene_skinned_renderer_t* slot = &scene->skinned_renderers[row];
    scene_skin_give(scene, slot);
    scene->starved_count -= slot->starved;
    *slot = *next;
    scene_skin_show(scene, row);
    scene->skins_pending |= next->storage == NULL;
}

bool
scene_renderer_bind_skin(scene_t* scene, scene_row_t row, const char* id, bool take, scene_failure_t* why) {
    const int renderer = scene_renderer_index(scene, row);
    if (renderer < 0) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, id);
    }
    /* Its normals are lit in its own frame, so a skinned renderer is a root. */
    if (scene_row_parent(scene, row) != SCENE_ROW_NONE) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_FORMAT, id);
    }
    scene_skinned_renderer_t next = {0};
    const asset_status_t status = open_renderer_skin(scene, row, renderer, id, &next);
    if (status != ASSET_OK) {
        return scene_report(why, SCENE_ERR_ASSET, status, id);
    }
    const bool lit = scene->light_count > 0 || scene->ambient_intensity > 0.0F;
    if (lit && next.bind.colors == NULL) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_FORMAT, SCENE_LIGHT_FAILURE_ALBEDO);
    }
    if (!take_skin_memory(scene, row, &next, lit, take)) {
        return scene_report(why, SCENE_ERR_MEMORY, ASSET_OK, id);
    }
    install_skin(scene, row, &next);
    return scene_report(why, SCENE_OK, ASSET_OK, id);
}

bool
renderer_set_skin(entity_t entity, const char* id, scene_failure_t* why) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(entity, &row);
    if (scene == NULL) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, id);
    }
    return scene_renderer_bind_skin(scene, row, id, true, why);
}

/* A renderer's textures: its mesh's LUVS sibling and the materials it names,
 * in one block, and their texels copied out of the pack into a second. */
typedef struct {
    r3d_mesh_texturing_t texturing;
    uint8_t* texels;
    r3d_texture_t materials[];
} renderer_texturing_t;

/* Copies the texels of `block`'s materials into one app-heap block. */
static bool
copy_texels(renderer_texturing_t* block) {
    size_t bytes = 0;
    for (int k = 0; k < block->texturing.material_count; k++) {
        bytes += r3d_texture_texel_bytes(&block->materials[k]);
    }
    block->texels = scene_allocate(bytes, MEMORY_APP);
    if (block->texels == NULL) {
        return false;
    }
    uint8_t* at = block->texels;
    for (int k = 0; k < block->texturing.material_count; k++) {
        at = r3d_texture_move_texels(&block->materials[k], at);
    }
    return true;
}

/* Opens the textures of `r`, whose mesh is the pack's `mesh_id`, into r->texturing. */
static bool
open_texturing(const scene_t* scene, scene_renderer_t* r, const char* mesh_id, scene_failure_t* why) {
    asset_view_t uvs;
    asset_status_t status = r3d_lit_mesh_open_sibling(scene->pack, mesh_id, R3D_TEXTURE_UVS_NAME_SUFFIX,
                                                      R3D_TEXTURE_UVS_ASSET, false, &uvs);
    if (status != ASSET_OK || uvs.data == NULL) {
        return scene_report(why, SCENE_ERR_ASSET, status == ASSET_OK ? ASSET_ERR_NOT_FOUND : status, mesh_id);
    }
    const int count = r3d_texture_uvs_material_count(&uvs);
    renderer_texturing_t* block =
        scene_allocate(sizeof *block + ((size_t)count * sizeof block->materials[0]), MEMORY_APP);
    if (block == NULL) {
        return scene_report(why, SCENE_ERR_MEMORY, ASSET_OK, mesh_id);
    }
    block->texels = NULL;
    status = r3d_texture_texturing_from_asset(&uvs, scene->pack, mesh_id, &r->mesh, block->materials, count,
                                              &block->texturing);
    if (status != ASSET_OK || !copy_texels(block)) {
        memory_free(block->texels);
        memory_free(block);
        return scene_report(why, status != ASSET_OK ? SCENE_ERR_ASSET : SCENE_ERR_MEMORY, status, mesh_id);
    }
    r->texturing = block;
    return scene_report(why, SCENE_OK, ASSET_OK, mesh_id);
}

bool
renderer_set_textured(entity_t entity, bool on, scene_failure_t* why) {
    scene_t* scene;
    const int i = renderer_of(entity, &scene);
    if (i < 0 || i >= scene->asset.renderer_count) {
        return scene_report(why, SCENE_ERR_ASSET, ASSET_ERR_NOT_FOUND, "");
    }
    scene_renderer_t* r = &scene->renderers[i];
    const char* mesh_id = scene->asset.renderers[i].mesh;
    if (on && r->texturing == NULL && !open_texturing(scene, r, mesh_id, why)) {
        return false;
    }
    renderer_texturing_t* block = r->texturing;
    /* Textures are lit by vertex colours: a flat-shaded mesh draws plain. */
    r->mesh.texturing = on && r->mesh.face_colors == NULL ? &block->texturing : NULL;
    r->changed = true;
    return scene_report(why, SCENE_OK, ASSET_OK, mesh_id);
}

void
scene_renderer_texturing_free(scene_renderer_t* r) {
    renderer_texturing_t* block = r->texturing;
    if (block != NULL) {
        memory_free(block->texels);
    }
    memory_free(block);
    r->texturing = NULL;
}
