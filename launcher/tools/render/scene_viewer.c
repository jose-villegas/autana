/* A scene-file harness: the scene manager owns camera time and drawing. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gfx/gfx.h"
#include "math/text/math_text.h"
#include "render/context/render_context.h"
#include "render_counts.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/renderer/renderer.h"
#include "scene/scene.h"
#include "scene/transform/transform.h"
#include "scene_viewer.h"

static const char* scene_id;
static const char* camera_name;
static char** object_names;
static int object_count;
static const char* textured_name; /* the renderer drawn with its textures, or NULL */
static int shell_quarter;
static recti_t size_rect;
static camera_present_t present;
static small_fill_t small_fill = SMALL_FILL_SMOOTH;
static int view = RENDER_DEBUG_VIEW_SHADED;
static const char* counts_path;
static FILE* counts;
static const char* poses_path;
static FILE* poses;
static entity_t shown;
static entity_t camera;

/* Every option back to what a fresh process starts with, for an open after a closed run. */
static void
reset_options(void) {
    scene_id = NULL;
    camera_name = NULL;
    object_names = NULL;
    object_count = 0;
    textured_name = NULL;
    size_rect = recti_empty();
    present = CAMERA_PRESENT_NEAREST;
    small_fill = SMALL_FILL_SMOOTH;
    view = RENDER_DEBUG_VIEW_SHADED;
    counts_path = NULL;
    poses_path = NULL;
}

static bool
options(int argc, char** argv) {
    reset_options();
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
        } else if (strcmp(flag, "--textured") == 0) {
            textured_name = value;
        } else if (strcmp(flag, "--size") == 0) {
            if (!recti_parse(value, NULL, &size_rect) || size_rect.x != 0 || size_rect.y != 0 || size_rect.w <= 0
                || size_rect.h <= 0 || size_rect.w > GFX_WIDTH || size_rect.h > GFX_HEIGHT) {
                fprintf(stderr, "scene_viewer: --size is WxH within %dx%d, not %s\n", GFX_WIDTH, GFX_HEIGHT, value);
                return false;
            }
        } else if (strcmp(flag, "--small-fill") == 0) {
            unsigned ceiling, gate, style;
            char trailing;
            if (sscanf(value, "%u,%u,%u%c", &ceiling, &gate, &style, &trailing) != 3 || ceiling > SMALL_FILL_CEILING_MAX
                || gate > UINT8_MAX || style >= SMALL_FILL_STYLE_COUNT) {
                fprintf(stderr, "scene_viewer: --small-fill is CEILING,GATE,STYLE within 0..%u,0..%u,0..%u\n",
                        SMALL_FILL_CEILING_MAX, UINT8_MAX, SMALL_FILL_STYLE_COUNT - 1);
                return false;
            }
            small_fill = (small_fill_t){(uint8_t)ceiling, (uint8_t)gate, (small_fill_style_t)style};
        } else if (strcmp(flag, "--present") == 0) {
            if (strcmp(value, "nearest") == 0) {
                present = CAMERA_PRESENT_NEAREST;
            } else if (strcmp(value, "scanline") == 0) {
                present = CAMERA_PRESENT_SCANLINE;
            } else if (strcmp(value, "double") == 0) {
                present = CAMERA_PRESENT_DOUBLE;
            } else {
                fprintf(stderr, "scene_viewer: --present is nearest, scanline or double\n");
                return false;
            }
        } else if (strcmp(flag, "--counts") == 0) {
            counts_path = value;
        } else if (strcmp(flag, "--poses") == 0) {
            poses_path = value;
        } else if (strcmp(flag, "--view") == 0) {
            view = render_context_debug_view_named(value);
            if (view == RENDER_DEBUG_VIEW_UNKNOWN) {
                fprintf(stderr, "scene_viewer: --view is ");
                render_context_print_debug_views(stderr);
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
    if (counts_path != NULL && view != render_context_debug_view_named("cull")) {
        fprintf(stderr, "scene_viewer: --counts needs --view cull\n");
        return false;
    }
    return true;
}

static bool
select_objects(entity_t scene) {
    for (int i = 0; i < object_count; i++) {
        const entity_t entity = entity_find(scene, name_hash(object_names[i]));
        if (!handle_is_none(entity) && renderer_mesh_id(entity) != NULL) {
            continue;
        }
        fprintf(stderr, "scene_viewer: unknown renderer '%s'; renderers:", object_names[i]);
        for (entity_t e = entity_next(scene, ENTITY_NONE); !handle_is_none(e); e = entity_next(scene, e)) {
            if (renderer_mesh_id(e) != NULL) {
                fprintf(stderr, " %s", entity_name(e));
            }
        }
        fprintf(stderr, "\n");
        return false;
    }
    if (object_count == 0) {
        return true;
    }
    for (entity_t entity = entity_next(scene, ENTITY_NONE); !handle_is_none(entity);
         entity = entity_next(scene, entity)) {
        if (renderer_mesh_id(entity) == NULL) {
            continue;
        }
        /* Enabled is parent AND own, so a named renderer needs its ancestors on and takes its descendants along. */
        bool enabled = false;
        for (int j = 0; j < object_count; j++) {
            const entity_t named = entity_find(scene, name_hash(object_names[j]));
            enabled |= entity_is_within(entity, named) || entity_is_within(named, entity);
        }
        entity_set_enabled(entity, enabled);
    }
    return true;
}

/* Draws the renderer --textured names with its textures. */
static bool
texture_object(entity_t scene) {
    if (textured_name == NULL) {
        return true;
    }
    scene_failure_t why;
    if (renderer_set_textured(entity_find(scene, name_hash(textured_name)), true, &why)) {
        return true;
    }
    fprintf(stderr, "scene_viewer: renderer '%s' has no textures to draw (status %d, asset %s)\n", textured_name,
            (int)why.status, asset_status_text(why.asset));
    return false;
}

/* The camera entity to activate: the named one, or the scene's first. */
static entity_t
chosen_camera(entity_t scene) {
    return camera_name != NULL ? entity_find(scene, name_hash(camera_name)) : camera_first(scene);
}

int
display_quarter_now(void) {
    return shell_quarter;
}

static bool
setup(int quarter) {
    shell_quarter = quarter;
    scene_failure_t why;
    entity_t scene = scene_load(scene_id);
    if (scene_state(scene) != PENDING_READY) {
        (void)scene_failure(scene, &why);
        fprintf(stderr, "scene_viewer: scene '%s' failed loading '%s' (status %d, asset %s)\n", scene_id, why.what,
                (int)why.status, asset_status_text(why.asset));
        scene_release(scene);
        return false;
    }
    const bool selected = select_objects(scene) && texture_object(scene);
    object_names = NULL;
    if (!selected || !camera_activate(chosen_camera(scene))) {
        if (selected) {
            fprintf(stderr, "scene_viewer: scene '%s' has no camera '%s'\n", scene_id,
                    camera_name == NULL ? "default" : camera_name);
        }
        scene_release(scene);
        return false;
    }
    if (view != RENDER_DEBUG_VIEW_SHADED) {
        bool has_mesh = false;
        for (entity_t e = entity_next(scene, ENTITY_NONE); !handle_is_none(e); e = entity_next(scene, e)) {
            has_mesh |= entity_enabled(e) && renderer_mesh_id(e) != NULL;
        }
        if (!has_mesh) {
            fprintf(stderr, "scene_viewer: scene '%s' has no enabled mesh for --view\n", scene_id);
            scene_release(scene);
            return false;
        }
    }
    for (entity_t e = entity_next(scene, ENTITY_NONE); !handle_is_none(e); e = entity_next(scene, e)) {
        (void)camera_set_present(e, present);
        (void)camera_set_small_fill(e, small_fill);
    }
    shown = scene;
    camera = chosen_camera(scene);
    /* What a caller sampling the path along its length reads. */
    fprintf(stderr, "scene_viewer: camera path %u ms\n", (unsigned)animator_clip_duration_ms(camera, 0));
    render_context_t* context = render_context_main();
    render_context_set_debug_view(context, view);
    if (counts_path != NULL && (counts = render_counts_open(counts_path, "scene_viewer")) == NULL) {
        scene_release(scene);
        return false;
    }
    if (poses_path != NULL
        && (poses = render_rows_open(poses_path, "frame,ms,px,py,pz,qx,qy,qz,qw", "scene_viewer")) == NULL) {
        if (counts != NULL) {
            fclose(counts);
            counts = NULL;
        }
        scene_release(scene);
        return false;
    }
    if (size_rect.w > 0) {
        const resolution_step_t size = {size_rect.w, size_rect.h};
        const resolution_config_t one = resolution_config(&size, 1, 1, INT32_MAX);
        render_context_set_dynamic_resolution(context, &one, NULL, 0);
    }
    return true;
}

/* One row per frame of what the cull view counted from the camera, and of where the camera stood. */
static void
draw(const provider_frame_t* frame) {
    const bool last = frame->index + 1 == frame->count;
    if (counts != NULL) {
        render_counts_row(&counts, frame);
    }
    if (poses != NULL) {
        const transformf_t world = transform_world(camera);
        fprintf(poses, "%d,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", frame->index, (unsigned)frame->elapsed_ms,
                (double)world.position.x, (double)world.position.y, (double)world.position.z, (double)world.rotation.x,
                (double)world.rotation.y, (double)world.rotation.z, (double)world.rotation.w);
        if (last) {
            fclose(poses);
            poses = NULL;
        }
    }
}

/* The rows a run that stopped early left open closed; render_host_close unloads the scene. */
static void
teardown(void) {
    if (counts != NULL) {
        fclose(counts);
        counts = NULL;
    }
    if (poses != NULL) {
        fclose(poses);
        poses = NULL;
    }
    shown = ENTITY_NONE;
    camera = ENTITY_NONE;
}

static uint32_t
path_ms(void) {
    return animator_clip_duration_ms(camera, 0);
}

static void
seek(uint32_t t_ms) {
    for (entity_t e = entity_next(shown, ENTITY_NONE); !handle_is_none(e); e = entity_next(shown, e)) {
        (void)animator_seek(e, ANIMATOR_CURRENT, t_ms);
    }
    /* Seeking cuts only when the active camera itself is animated; objects
     * or a camera parent moving under a still camera need the cut too. */
    render_context_forget(render_context_main());
}

const frame_provider_t scene_provider = {
    .name = "scene_viewer",
    .quarter = 1,
    .frames = 30,
    .dt_ms = 16,
    .options = options,
    .setup = setup,
    .draw = draw,
    .teardown = teardown,
    .path_ms = path_ms,
    .seek = seek,
    .render_context = render_context_main,
};
