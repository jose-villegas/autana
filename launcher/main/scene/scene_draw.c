/*
 * scene_draw: draws every ready scene through the active camera, once per
 * frame, into the engine's render context, which decides the size and
 * quality. A scene whose root is disabled is not drawn.
 */
#include <assert.h>
#include <stdlib.h>

#include "esp_log.h"
#include "hash/name_hash64.h"

#include "render/context/render_context.h"
#include "render/lod/r3d_lod.h"
#include "render/r3d_pipeline.h"
#include "scene/camera/camera.h"
#include "scene/scene_internal.h"

#pragma GCC diagnostic error "-Wdouble-promotion"

typedef struct {
    scene_t* scene;
    int camera; /* index into the scene's cameras */
} active_t;

static active_t active;
/* The renderers the last frame drew, hashed (0 before one), and this
 * frame's changed boxes (render/partial). */
static name_hash64_t drawn_set;
static box3f_t changed[RENDER_PARTIAL_CHANGES_MAX];
static bool paused;
static bool expanded;
static bool stepped;  /* scene_render() ran, and scene_compose() has not yet */
static bool rendered; /* it drew, and scene_compose() has not upscaled it */
static bool refused;  /* the context had no scratch for the active camera, logged once */
/* The active camera's view: here, not on the draw's stack. */
static render_view_t drawn_view;
/* The scenes this frame draws: the active camera's first, then every other
 * ready one whose root is enabled, by root slot. */
static scene_t* drawn_scenes[SCENE_ROOTS_MAX];
static int drawn_scene_count;

/* What the raster draws this frame, of every drawn scene: one block, the
 * instances then the batch draws, sized to the drawn scenes' renderers and
 * batches summed and only ever grown while they stay. From the scenes' own
 * memory (scene_allocate_kept()), so a lent scene's comes from its lender:
 * dropped when any scene goes, before its lender can take it back. */
static struct {
    void* block;
    r3d_instance_t* instances;
    r3d_batch_draw_t* batch_draws;
    int instance_capacity, batch_capacity;
} drawn;

bool
scene_has_active_camera(void) {
    return active.scene != NULL && (!paused || expanded);
}

void
scene_set_paused(bool pause) {
    paused = pause;
    rendered = false;
    stepped = false;
}

void
scene_draw_activate(scene_t* scene, int camera) {
    active = (active_t){scene, camera};
    drawn_set = 0;
    render_context_forget(render_context_main());
    refused = false;
    rendered = false;
    stepped = false;
    expanded = false;
}

void
camera_deactivate(void) {
    active = (active_t){NULL, 0};
    rendered = false;
    stepped = false;
    expanded = false;
}

static void
drop_drawn(void) {
    scene_free_kept(drawn.block);
    drawn = (__typeof__(drawn)){0};
}

void
scene_draw_forget(const scene_t* scene) {
    drop_drawn();
    if (active.scene == scene) {
        camera_deactivate();
    }
}

void
scene_draw_release(void) {
    camera_deactivate();
    drop_drawn();
    render_context_release(render_context_main());
    paused = false;
}

void
scene_draw_camera_cut(const scene_t* scene, scene_row_t entity) {
    if (active.scene == scene && scene->cameras[active.camera].entity == entity) {
        render_context_forget(render_context_main());
    }
}

static scene_camera_t*
active_camera(void) {
    return &active.scene->cameras[active.camera];
}

entity_t
camera_active(void) {
    return active.scene == NULL ? ENTITY_NONE : scene_row_entity(active.scene, active_camera()->entity);
}

void
camera_cut(entity_t camera) {
    scene_row_t row;
    const scene_t* scene = scene_entity_row(camera, &row);
    if (scene != NULL) {
        scene_draw_camera_cut(scene, row);
    }
}

static void
list_drawn_scenes(void) {
    drawn_scenes[0] = active.scene;
    drawn_scene_count = 1;
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_t* scene = scene_ready_at(i);
        if (scene != NULL && scene != active.scene && (scene->root->flags & SCENE_FLAG_ACTIVE) != 0) {
            drawn_scenes[drawn_scene_count++] = scene;
        }
    }
}

/* Grows `drawn` to the drawn scenes' renderers and batches; false when that
 * found no memory. */
static bool
fit_drawn(void) {
    int instances = 0;
    int batches = 0;
    for (int k = 0; k < drawn_scene_count; k++) {
        instances += drawn_scenes[k]->renderer_count;
        batches += drawn_scenes[k]->asset.batch_count;
    }
    if (instances <= drawn.instance_capacity && batches <= drawn.batch_capacity) {
        return true;
    }
    instances = instances > drawn.instance_capacity ? instances : drawn.instance_capacity;
    batches = batches > drawn.batch_capacity ? batches : drawn.batch_capacity;
    size_t end = sizeof(r3d_instance_t) * (size_t)instances;
    const size_t at = scene_storage_carve(&end, sizeof(r3d_batch_draw_t) * (size_t)batches, _Alignof(r3d_batch_draw_t));
    scene_free_kept(drawn.block);
    drawn.block = scene_allocate_kept(end);
    const bool fits = drawn.block != NULL;
    drawn.instances = drawn.block;
    drawn.batch_draws = fits ? (r3d_batch_draw_t*)(void*)((uint8_t*)drawn.block + at) : NULL;
    drawn.instance_capacity = fits ? instances : 0;
    drawn.batch_capacity = fits ? batches : 0;
    return fits;
}

const r3d_instance_t*
scene_drawn_instances(void) {
    return drawn.instances;
}

/* `scene`'s active renderers, appended to `drawn` from `*count` instances
 * and `*batches` batch draws. */
static void
fill_scene(scene_t* scene, const r3d_lod_measure_t* measure, int* count, int* batches) {
    for (int i = 0; i < scene->active_renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[scene->active_renderers[i]];
        if (r->mesh.node_count == 0) {
            continue;
        }
        if (scene->skinned_renderers[r->entity].bound) {
            scene->animators[r->entity].drawn_radius_px = r3d_lod_bounds_pixels(r->world_bounds, measure);
        }
        const r3d_instance_t instance = {&r->mesh, r->placed ? &r->model : NULL, &r->world_bounds, r->lod_coarse};
        if (r->batch != NULL) {
            drawn.batch_draws[(*batches)++] = (r3d_batch_draw_t){instance, r->batch, 0};
        } else {
            drawn.instances[(*count)++] = instance;
        }
    }
}

/* Every drawn scene's active renderers with something to draw, as the raster
 * draws them, into `drawn`: the instances, returning their count, and the
 * batches, `*batches` of them. scene_resolve() has listed them. */
static int
fill_instances(int* batches) {
    r3d_lens_t lens;
    r3d_lens_init(&lens, &drawn_view, 1);
    const r3d_lod_measure_t measure = r3d_lod_measure(&lens);
    int count = 0;
    *batches = 0;
    for (int k = 0; k < drawn_scene_count; k++) {
        fill_scene(drawn_scenes[k], &measure, &count, batches);
    }
    return count;
}

/* `scene`'s part of fill_changed(): its changed renderers' boxes into
 * `changed` from `changes` on (-1 once more than fit), each drawn renderer
 * hashed into `*set` with its root slot. Returns the new `changes`. */
static int
fill_scene_changed(scene_t* scene, int changes, name_hash64_t* set) {
    const int slot = (int)(scene->root - scene_roots);
    for (int i = 0; i < scene->active_renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[scene->active_renderers[i]];
        if (r->mesh.node_count == 0) {
            continue;
        }
        *set = name_hash64_n(*set, &slot, sizeof slot);
        *set = name_hash64_n(*set, &scene->active_renderers[i], sizeof scene->active_renderers[i]);
        if (r->changed && changes >= 0) {
            changes = changes < RENDER_PARTIAL_CHANGES_MAX ? changes : -1;
            if (changes >= 0) {
                changed[changes++] = box3f_union(r->drawn_bounds, r->world_bounds);
            }
        }
        r->changed = false;
        r->drawn_bounds = r->world_bounds;
    }
    return changes;
}

/* Where each changed renderer was and is, for partial redraw; -1 when the
 * drawn set changed (a renderer left or joined) or more changed than fit.
 * Every drawn renderer's box is kept as where it was drawn. */
static __attribute__((noinline)) int
fill_changed(void) {
    int changes = 0;
    name_hash64_t set = NAME_HASH64_OFFSET_BASIS;
    for (int k = 0; k < drawn_scene_count; k++) {
        changes = fill_scene_changed(drawn_scenes[k], changes, &set);
    }
    const bool same = set == drawn_set;
    drawn_set = set;
    return same ? changes : -1;
}

/* The active camera's view from its entity's world, into `drawn_view`. Its own
 * frame, gone before the draw: the pose and the world walk stay off the
 * draw's stack. */
static __attribute__((noinline)) void
active_view(const scene_t* scene, const scene_camera_t* camera, viewport_t viewport) {
    const transformf_t pose = scene_row_world(scene->root, camera->entity);
    drawn_view = lens_view(&camera->lens, &pose, viewport);
}

box3f_t
scene_bounds(entity_t entity) {
    scene_row_t row;
    const scene_root_t* record = scene_entity_lookup(entity, &row);
    scene_t* scene = record == NULL ? NULL : record->scene;
    if (scene == NULL) {
        return box3f_empty();
    }
    scene_resolve(scene);
    box3f_t bounds = box3f_empty();
    for (int i = 0; i < scene->active_renderer_count; i++) {
        const scene_renderer_t* r = &scene->renderers[scene->active_renderers[i]];
        if (r->mesh.node_count > 0) {
            bounds = box3f_union(bounds, r->world_bounds);
        }
    }
    return bounds;
}

static void
draw_active(int quarter, int width, int height) {
    scene_t* scene = active.scene;
    const scene_camera_t* camera = active_camera();
    list_drawn_scenes();
    for (int k = 0; k < drawn_scene_count; k++) {
        scene_resolve(drawn_scenes[k]);
    }
    /* Size measurements must use this draw's lens, including its viewport. */
    active_view(scene, camera, (viewport_t){width, height, quarter});
    int batches = 0;
    /* -1: no memory for the list. */
    const int count = fit_drawn() ? fill_instances(&batches) : -1;
    if (count + batches == 0) {
        return;
    }
    if (count >= 0) {
        render_context_set_changed(render_context_main(), changed, fill_changed());
        render_context_set_batches(render_context_main(), drawn.batch_draws, batches);
        rendered = render_context_draw(render_context_main(), drawn.instances, count, &drawn_view, camera->clear);
    }
    if (!rendered && !refused) {
        ESP_LOGE("scene", "no memory to draw '%s'; it stays blank", scene->root->id);
        refused = true;
    }
}

void
scene_render(uint32_t dt_ms, int quarter, int width, int height) {
    if (paused) {
        return;
    }
    stepped = true;
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_t* scene = scene_ready_at(i);
        if (scene != NULL) {
            scene_animation_step(scene, dt_ms);
        }
    }
    if (active.scene != NULL) {
        draw_active(quarter, width, height);
    }
}

bool
scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target) {
    if (target->half_pixels == NULL) {
        expanded = false;
    }
    if (paused) {
        return expanded;
    }
    if (!stepped) {
        scene_render(dt_ms, quarter, target->width, target->height);
    }
    if (rendered) {
        expanded =
            render_context_compose(render_context_main(), target->pixels, target->half_pixels, target->half_seams);
    }
    rendered = false;
    stepped = false;
    return expanded;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
scene_draw_checkpoint_capture(checkpoint_t* saved) {
    CHECKPOINT_CAPTURE(saved, active);
    CHECKPOINT_CAPTURE(saved, paused);
    CHECKPOINT_CAPTURE(saved, expanded);
    CHECKPOINT_CAPTURE(saved, stepped);
    CHECKPOINT_CAPTURE(saved, rendered);
    CHECKPOINT_CAPTURE(saved, refused);
    scene_world_checkpoint_capture(saved);
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_t* scene = scene_ready_at(i);
        if (scene != NULL) {
            scene_checkpoint_capture(saved, scene);
        }
    }
}
#endif
