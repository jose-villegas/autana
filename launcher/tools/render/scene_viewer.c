/* A scene-file harness: the scene manager owns camera time and drawing. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gfx/gfx.h"
#include "render/context/render_context.h"
#include "render_host.h"
#include "scene/scene.h"

static const char* scene_id;
static const char* camera_name;
static char** object_names;
static int object_count;
static int shell_quarter;
static int size_width, size_height;
static int view = RENDER_VIEW_SHADED;

static bool
options(int argc, char** argv) {
    object_names = argv;
    for (int i = 0; i < argc; i += 2) {
        const char* flag = argv[i];
        if (i + 1 >= argc) {
            fprintf(stderr, "scene_viewer: %s needs a value\n", flag);
            return false;
        }
        const char* value = argv[i + 1];
        if (strcmp(flag, "--scene") == 0) {
            scene_id = value;
        } else if (strcmp(flag, "--camera") == 0) {
            camera_name = value;
        } else if (strcmp(flag, "--object") == 0) {
            object_names[object_count++] = argv[i + 1];
        } else if (strcmp(flag, "--size") == 0) {
            char trailing;
            if (sscanf(value, "%dx%d%c", &size_width, &size_height, &trailing) != 2 || size_width <= 0
                || size_height <= 0 || size_width > GFX_WIDTH || size_height > GFX_HEIGHT) {
                fprintf(stderr, "scene_viewer: --size is WxH within %dx%d, not %s\n", GFX_WIDTH, GFX_HEIGHT, value);
                return false;
            }
        } else if (strcmp(flag, "--view") == 0) {
            view = render_context_view_named(value);
            if (view == RENDER_VIEW_UNKNOWN) {
                fprintf(stderr, "scene_viewer: --view is shaded");
                for (int k = 0; k < RENDER_VIEW_COUNT; k++) {
                    fprintf(stderr, ", %s", render_context_view(k)->name);
                }
                fprintf(stderr, ", not %s\n", value);
                return false;
            }
        } else {
            fprintf(stderr, "scene_viewer: unknown option %s\n", flag);
            return false;
        }
    }
    if (scene_id == NULL) {
        fprintf(stderr, "scene_viewer: needs --scene ID\n");
        return false;
    }
    return true;
}

static bool
select_objects(scene_t* scene) {
    for (int i = 0; i < object_count; i++) {
        const scene_entity_t entity = scene_find(scene, object_names[i]);
        if (entity != SCENE_ENTITY_NONE && scene_entity_mesh_id(scene, entity) != NULL) {
            continue;
        }
        fprintf(stderr, "scene_viewer: unknown renderer '%s'; renderers:", object_names[i]);
        for (int j = 0; j < scene_entity_count(scene); j++) {
            if (scene_entity_mesh_id(scene, (scene_entity_t)j) != NULL) {
                fprintf(stderr, " %s", scene_entity_name(scene, (scene_entity_t)j));
            }
        }
        fprintf(stderr, "\n");
        return false;
    }
    if (object_count == 0) {
        return true;
    }
    for (int i = 0; i < scene_entity_count(scene); i++) {
        const scene_entity_t entity = (scene_entity_t)i;
        if (scene_entity_mesh_id(scene, entity) == NULL) {
            continue;
        }
        bool enabled = false;
        for (int j = 0; j < object_count; j++) {
            enabled |= strcmp(scene_entity_name(scene, entity), object_names[j]) == 0;
        }
        scene_entity_set_enabled(scene, entity, enabled);
    }
    return true;
}

int
display_quarter_now(void) {
    return shell_quarter;
}

static bool
setup(int quarter) {
    shell_quarter = quarter;
    scene_failure_t why;
    scene_t* scene = scene_load(scene_id, &why);
    if (scene == NULL) {
        fprintf(stderr, "scene_viewer: scene '%s' failed loading '%s' (status %d, asset %s)\n", scene_id, why.what,
                (int)why.status, asset_status_text(why.asset));
        return false;
    }
    const bool selected = select_objects(scene);
    object_names = NULL;
    if (!selected || !scene_activate(scene, camera_name)) {
        if (selected) {
            fprintf(stderr, "scene_viewer: scene '%s' has no camera '%s'\n", scene_id,
                    camera_name == NULL ? "default" : camera_name);
        }
        scene_unload(scene);
        return false;
    }
    if (view != RENDER_VIEW_SHADED) {
        bool has_mesh = false;
        for (int i = 0; i < scene_entity_count(scene); i++) {
            const scene_entity_t entity = (scene_entity_t)i;
            has_mesh |= scene_entity_enabled(scene, entity) && scene_entity_mesh_id(scene, entity) != NULL;
        }
        if (!has_mesh) {
            fprintf(stderr, "scene_viewer: scene '%s' has no enabled mesh for --view\n", scene_id);
            scene_unload(scene);
            return false;
        }
    }
    render_context_t* context = render_context_main();
    render_context_set_view(context, view);
    if (size_width > 0) {
        const resolution_step_t size = {size_width, size_height};
        const resolution_config_t one = resolution_config(&size, 1, 1, INT32_MAX);
        render_context_set_dynamic_resolution(context, &one, NULL, 0);
    }
    return true;
}

static void
draw(const render_frame_t* frame) {
    (void)frame;
}

const render_scene_t render_scene = {
    .name = "scene_viewer",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
};
