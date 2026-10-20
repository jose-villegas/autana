/* Host bridge: scene entity APIs through the animation and skin systems, without drawing. */
#include <stdio.h>
#include <stdlib.h>
#include "asset/asset_file.h"
#include "scene/scene_internal.h"

enum { ARG_COUNT = 7, ARG_PACK = 1, ARG_SKELETON, ARG_SKIN, ARG_MESH, ARG_CLIP, ARG_TIME };

#define HOST_SCENE_ID    "scene"
#define HOST_ENTITY_NAME "subject"

/* scene_draw.c stand-ins: without its scene_activate() no scene is active, where its versions do nothing too;
 * linking scene_draw.c needs the whole renderer source list. Any drawing call still fails to link. */
void
scene_draw_forget(const scene_t* scene) {
    (void)scene;
}

void
scene_draw_release(void) {}

void
scene_draw_camera_cut(const scene_t* scene, scene_entity_t entity) {
    (void)scene;
    (void)entity;
}

static bool
sample(const asset_pack_t* pack, char** argv) {
    scene_failure_t why;
    scene_t* scene = scene_load_from(pack, HOST_SCENE_ID, &why);
    if (scene == NULL) {
        return false;
    }
    const scene_entity_t entity = scene_find(scene, HOST_ENTITY_NAME);
    const char* clips[] = {argv[ARG_CLIP]};
    bool ok =
        scene_animator_set_clips(scene, entity, argv[ARG_SKELETON], clips, 1, &why)
        && scene_renderer_set_skin(scene, entity, argv[ARG_SKIN], &why) && scene_animator_play(scene, entity, 0, 0)
        && scene_animator_seek(scene, entity, SCENE_ANIMATOR_CURRENT, (uint32_t)strtoul(argv[ARG_TIME], NULL, 10));
    if (ok) {
        const r3d_lit_mesh_t* mesh = scene_entity_mesh(scene, entity);
        for (int v = 0; v < mesh->vertex_count; v++) {
            printf("%d %d %d\n", mesh->positions[v][0], mesh->positions[v][1], mesh->positions[v][2]);
        }
    } else {
        fprintf(stderr, "scene animation: %d %s %s\n", why.status, asset_status_text(why.asset), why.what);
    }
    scene_unload(scene);
    return ok;
}

int
main(int argc, char** argv) {
    if (argc != ARG_COUNT) {
        fprintf(stderr, "skin_host PACK SKELETON SKIN MESH CLIP TIME_MS\n");
        return EXIT_FAILURE;
    }
    asset_pack_t pack;
    void* buffer;
    if (asset_file_open(argv[ARG_PACK], &pack, &buffer) != ASSET_OK) {
        return EXIT_FAILURE;
    }
    const bool ok = sample(&pack, argv);
    asset_file_release(buffer);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
