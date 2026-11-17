/* A scene-file harness: the scene manager owns camera time and drawing. */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "gfx/gfx.h"
#include "math/text/math_text.h"
#include "render/context/render_context.h"
#include "render/view/raster_cull_view.h"
#include "scene/scene.h"
#include "scene_viewer.h"

static const char* scene_id;
static const char* camera_name;
static char** object_names;
static int object_count;
static int shell_quarter;
static recti_t size_rect;
static camera_present_t present;
static small_fill_t small_fill = SMALL_FILL_SMOOTH;
static int view = RENDER_DEBUG_VIEW_SHADED;
static const char* counts_path;
static FILE* counts;
static const char* poses_path;
static FILE* poses;
static scene_t* shown;
static scene_entity_t camera;

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
select_objects(scene_t* scene) {
    for (int i = 0; i < object_count; i++) {
        const scene_entity_t entity = scene_find(scene, name_hash(object_names[i]));
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
        /* Enabled is parent AND own, so a named renderer needs its ancestors on and takes its descendants along. */
        bool enabled = false;
        for (int j = 0; j < object_count; j++) {
            const scene_entity_t named = scene_find(scene, name_hash(object_names[j]));
            enabled |= scene_entity_is_within(scene, entity, named) || scene_entity_is_within(scene, named, entity);
        }
        scene_entity_set_enabled(scene, entity, enabled);
    }
    return true;
}

/* The camera entity scene_activate() chose: the named one, or the first whose lens is the first camera's. */
static scene_entity_t
active_camera(const scene_t* scene) {
    if (camera_name != NULL) {
        return scene_find(scene, name_hash(camera_name));
    }
    const camera_t* first = scene_camera_lens(scene, SCENE_CAMERA_FIRST);
    for (int i = 0; i < scene_entity_count(scene); i++) {
        if (scene_camera_lens(scene, name_hash(scene_entity_name(scene, (scene_entity_t)i))) == first) {
            return (scene_entity_t)i;
        }
    }
    return SCENE_ENTITY_NONE;
}

/* A per-frame CSV with its header written; NULL, said on stderr, when it cannot be made. */
static FILE*
open_rows(const char* path, const char* header) {
    FILE* rows = fopen(path, "w");
    if (rows == NULL) {
        fprintf(stderr, "scene_viewer: cannot write %s\n", path);
        return NULL;
    }
    fprintf(rows, "%s\n", header);
    return rows;
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
    if (!selected || !scene_activate(scene, camera_name == NULL ? SCENE_CAMERA_FIRST : name_hash(camera_name))) {
        if (selected) {
            fprintf(stderr, "scene_viewer: scene '%s' has no camera '%s'\n", scene_id,
                    camera_name == NULL ? "default" : camera_name);
        }
        scene_unload(scene);
        return false;
    }
    if (view != RENDER_DEBUG_VIEW_SHADED) {
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
    for (int i = 0; i < scene_entity_count(scene); i++) {
        (void)scene_camera_set_present(scene, (scene_entity_t)i, present);
        (void)scene_camera_set_small_fill(scene, (scene_entity_t)i, small_fill);
    }
    shown = scene;
    camera = active_camera(scene);
    /* What a caller sampling the path along its length reads. */
    fprintf(stderr, "scene_viewer: camera path %u ms\n", (unsigned)scene_animator_clip_duration_ms(scene, camera, 0));
    render_context_t* context = render_context_main();
    render_context_set_debug_view(context, view);
#define COUNT_NAME(name, member) "," #name
    if (counts_path != NULL
        && (counts = open_rows(counts_path, "frame,ms" RASTER_CULL_FRAME_FIELDS(COUNT_NAME))) == NULL) {
        scene_unload(scene);
        return false;
    }
    if (poses_path != NULL && (poses = open_rows(poses_path, "frame,ms,px,py,pz,qx,qy,qz,qw")) == NULL) {
        scene_unload(scene);
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
        const raster_cull_frame_t f = raster_cull_frame(render_context_debug_view_state(render_context_main()));
        fprintf(counts, "%d,%u", frame->index, (unsigned)frame->elapsed_ms);
#define COUNT_VALUE(name, member) fprintf(counts, ",%d", f.member);
        RASTER_CULL_FRAME_FIELDS(COUNT_VALUE)
        fputc('\n', counts);
        if (last) {
            fclose(counts);
            counts = NULL;
        }
    }
    if (poses != NULL) {
        const transformf_t world = scene_entity_world(shown, camera);
        fprintf(poses, "%d,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n", frame->index, (unsigned)frame->elapsed_ms,
                (double)world.position.x, (double)world.position.y, (double)world.position.z, (double)world.rotation.x,
                (double)world.rotation.y, (double)world.rotation.z, (double)world.rotation.w);
        if (last) {
            fclose(poses);
            poses = NULL;
        }
    }
}

static uint32_t
path_ms(void) {
    return scene_animator_clip_duration_ms(shown, camera, 0);
}

static void
seek(uint32_t t_ms) {
    for (int i = 0; i < scene_entity_count(shown); i++) {
        (void)scene_animator_seek(shown, (scene_entity_t)i, SCENE_ANIMATOR_CURRENT, t_ms);
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
    .path_ms = path_ms,
    .seek = seek,
    .render_context = render_context_main,
};
