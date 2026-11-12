/* Host bridge: scene entity APIs through the animation and skin systems, without drawing. */
#include <stdio.h>
#include <stdlib.h>
#include "app/app_arena.h"
#include "asset/asset_file.h"
#include "scene/animator/animator.h"
#include "scene/renderer/renderer.h"
#include "scene/scene_internal.h"

enum { ARG_COUNT = 9, ARG_PACK = 1, ARG_SCENE, ARG_ENTITY, ARG_SKELETON, ARG_SKIN, ARG_MESH, ARG_CLIP, ARG_TIME };

static bool
sample(const asset_pack_t* pack, char** argv) {
    scene_failure_t why;
    entity_t scene = scene_load_from(pack, argv[ARG_SCENE]);
    if (scene_state(scene) != PENDING_READY) {
        (void)scene_failure(scene, &why);
        fprintf(stderr, "scene load: %d %s %s\n", why.status, asset_status_text(why.asset), why.what);
        scene_release(scene);
        return false;
    }
    const entity_t entity = entity_find(scene, name_hash(argv[ARG_ENTITY]));
    const char* clips[] = {argv[ARG_CLIP]};
    bool ok = animator_set_clips(entity, argv[ARG_SKELETON], clips, 1, &why)
              && renderer_set_skin(entity, argv[ARG_SKIN], &why) && animator_play(entity, 0, 0)
              && animator_seek(entity, ANIMATOR_CURRENT, (uint32_t)strtoul(argv[ARG_TIME], NULL, 10));
    if (ok) {
        const r3d_lit_mesh_t* mesh = renderer_mesh(entity);
        for (int v = 0; v < mesh->vertex_count; v++) {
            printf("%d %d %d\n", mesh->positions[v][0], mesh->positions[v][1], mesh->positions[v][2]);
        }
    } else {
        fprintf(stderr, "scene animation: %d %s %s\n", why.status, asset_status_text(why.asset), why.what);
    }
    scene_release(scene);
    return ok;
}

int
main(int argc, char** argv) {
    if (argc != ARG_COUNT) {
        fprintf(stderr, "skin_host PACK SCENE ENTITY SKELETON SKIN MESH CLIP TIME_MS\n");
        return EXIT_FAILURE;
    }
    /* The scene takes its blocks from the app heap, as on the board, where the shell enters it for each app visit. */
    if (!app_arena_init()) {
        fprintf(stderr, "skin_host: the app region does not fit\n");
        return EXIT_FAILURE;
    }
    app_arena_enter();
    asset_pack_t pack;
    void* buffer;
    bool ok = asset_file_open(argv[ARG_PACK], &pack, &buffer) == ASSET_OK;
    if (ok) {
        ok = sample(&pack, argv);
        asset_file_release(buffer);
    }
    app_arena_exit();
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
