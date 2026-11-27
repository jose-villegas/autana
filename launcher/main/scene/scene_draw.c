/*
 * scene_draw: draws every ready scene through the active camera, once per
 * frame, into the engine's render context, which decides the size and
 * quality. A scene whose root is disabled is not drawn.
 */
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "core/app_heap.h"
#include "esp_log.h"
#include "hash/name_hash64.h"

#include "render/contact/contact_blob.h"
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
static bool contact_blobs;
static bool refused; /* the context had no scratch for the active camera, logged once */
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
 * dropped when any scene goes, before its lender can take it back. A lender
 * does not free, so a block outgrown while the scenes stay waits in its arena
 * until then. */
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
    render_context_skip(render_context_main()); /* the paused time is not a frame */
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
    for (int i = 0; i < drawn_scene_count; i++) {
        if (drawn_scenes[i] == scene) {
            memmove(&drawn_scenes[i], &drawn_scenes[i + 1], (size_t)(drawn_scene_count - i - 1) * sizeof *drawn_scenes);
            drawn_scene_count--;
            break;
        }
    }
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
    const size_t at = mem_carve(&end, sizeof(r3d_batch_draw_t) * (size_t)batches, _Alignof(r3d_batch_draw_t));
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

/* The drawn view's size measure, built at its first use in a frame. */
typedef struct {
    r3d_lod_measure_t measure;
    bool ready;
} size_measure_t;

/* `bounds`' drawn radius in pixels, through `m`'s measure. */
static float
drawn_radius(box3f_t bounds, size_measure_t* m) {
    if (!m->ready) {
        r3d_lens_t lens;
        r3d_lens_init(&lens, &drawn_view, 1);
        m->measure = r3d_lod_measure(&lens);
        m->ready = true;
    }
    return r3d_lod_bounds_pixels(bounds, &m->measure);
}

/* `scene`'s active renderers, appended to `drawn` from `*count` instances
 * and `*batches` batch draws. A skinned copy is measured only when its next
 * step picks its tier. */
static void
fill_scene(scene_t* scene, size_measure_t* measure, int* count, int* batches) {
    for (int i = 0; i < scene->active_renderer_count; i++) {
        scene_renderer_t* r = &scene->renderers[scene->active_renderers[i]];
        if (r->mesh.node_count == 0) {
            continue;
        }
        scene_animator_t* a = &scene->animators[r->entity];
        if (scene->skinned_renderers[r->entity].bound && scene_animator_due(scene, a)) {
            a->drawn_radius_px = drawn_radius(r->world_bounds, measure);
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
    size_measure_t measure = {.ready = false};
    int count = 0;
    *batches = 0;
    for (int k = 0; k < drawn_scene_count; k++) {
        fill_scene(drawn_scenes[k], &measure, &count, batches);
    }
    return count;
}

_Static_assert(SKIN_CONTACTS_MAX <= CONTACT_BLOBS_MAX, "footprints hold every selected contact joint");

#define CONTACT_AXIS_EPSILON 0.000001F

vec2f_t
scene_contact_reach(const scene_t* scene, const scene_renderer_t* r) {
    const vec3f_t half = box3f_half_extent(r->world_bounds);
    const float radius = fminf(half.x, half.z);
    const bool silhouette = r->contact.shape == SCENE_CONTACT_SILHOUETTE
                            && scene->skinned_renderers[r->entity].contact_mask.coverage != NULL;
    return (vec2f_t){silhouette ? half.x : radius, silhouette ? half.z : radius};
}

static vec3f_t
warp_axis(vec3f_t light) {
    const vec3f_t flat = {-light.x, 0, -light.z};
    return vec3f_dot(flat, flat) > CONTACT_AXIS_EPSILON * CONTACT_AXIS_EPSILON ? vec3f_normalize(flat) : vec3f_zero();
}

static float
light_elevation(vec3f_t light) {
    return mathf_clamp(light.y, CONTACT_LIGHT_ELEVATION_MIN, 1);
}

static contact_blob_t
warped_blob(box3f_t mover, float ground_y, vec2f_t reach, vec3f_t light) {
    const vec3f_t axis = warp_axis(light);
    const float e = vec3f_dot(axis, axis) == 0 ? 1 : light_elevation(light);
    vec3f_t centre = box3f_centre(mover);
    centre.y = ground_y;
    const float radius = fminf(reach.x, reach.y);
    centre = vec3f_add(centre, vec3f_scale(axis, radius * sqrtf(1 - (e * e)) / e));
    const vec3f_t u = {reach.x, 0, 0};
    const vec3f_t v = {0, 0, reach.y};
    return (contact_blob_t){.centre = centre,
                            .u = vec3f_add(u, vec3f_scale(axis, vec3f_dot(u, axis) * (1 / e - 1))),
                            .v = vec3f_add(v, vec3f_scale(axis, vec3f_dot(v, axis) * (1 / e - 1)))};
}

bool
scene_contact_blob(const scene_t* scene, const scene_renderer_t* r, vec3f_t light, contact_blob_t* blob) {
    const vec2f_t reach = scene_contact_reach(scene, r);
    if (r->contact.warp) {
        *blob = warped_blob(r->world_bounds, r->contact.ground_y, reach, light);
    } else {
        vec3f_t centre = box3f_centre(r->world_bounds);
        centre.y = r->contact.ground_y;
        *blob = (contact_blob_t){.centre = centre, .u = {reach.x, 0, 0}, .v = {0, 0, reach.y}};
    }
    blob->strength = r->contact.strength;
    if (r->contact.shape == SCENE_CONTACT_SILHOUETTE) {
        const contact_mask_t* mask = &scene->skinned_renderers[r->entity].contact_mask;
        blob->mask = mask->coverage != NULL ? mask : NULL;
    }
    return reach.x > 0 && reach.y > 0 && blob->strength > 0;
}

int
scene_contact_blobs(const scene_t* scene, const scene_renderer_t* r, vec3f_t light, contact_blob_t* blobs) {
    if (r->contact.shape != SCENE_CONTACT_FEET) {
        return scene_contact_blob(scene, r, light, blobs) ? 1 : 0;
    }
    const scene_skinned_renderer_t* skin = &scene->skinned_renderers[r->entity];
    if (skin->storage == NULL || r->contact.strength <= 0) {
        return 0;
    }
    const mat4f_t world = scene_row_world_matrix(scene->root, r->entity);
    const vec3f_t scale = scene_row_world(scene->root, r->entity).scale;
    const float radius = skin->contacts.radius * fmaxf(fabsf(scale.x), fabsf(scale.z));
    if (!(radius > 0)) {
        return 0;
    }
    for (int i = 0; i < skin->contacts.count; i++) {
        const vec3f_t joint = mat4f_apply(&skin->model[skin->contacts.joints[i]], vec3f_zero());
        const vec3f_t centre = mat4f_apply(&world, joint);
        blobs[i] = warped_blob(box3f_from_point(centre), r->contact.ground_y, (vec2f_t){radius, radius},
                               r->contact.warp ? light : vec3f_unit_y());
        blobs[i].strength = r->contact.strength;
    }
    return skin->contacts.count;
}

box3f_t
scene_contact_bounds(const scene_t* scene, scene_renderer_t* r, vec3f_t light) {
    contact_blob_t blobs[CONTACT_BLOBS_MAX];
    const int count = scene_contact_blobs(scene, r, light, blobs);
    box3f_t bounds = box3f_empty();
    for (int i = 0; i < count; i++) {
        const contact_blob_t* blob = &blobs[i];
        const vec3f_t extent = {fabsf(blob->u.x) + fabsf(blob->v.x), 0, fabsf(blob->u.z) + fabsf(blob->v.z)};
        bounds = box3f_union(bounds, (box3f_t){vec3f_sub(blob->centre, extent), vec3f_add(blob->centre, extent)});
    }
    return bounds;
}

static __attribute__((noinline)) box3f_t
fill_contact_bounds(scene_t* scene, scene_renderer_t* r) {
    if (r->changed && r->contact.shape == SCENE_CONTACT_SILHOUETTE) {
        scene_skin_mask_refresh(scene, r);
    }
    const vec3f_t light =
        r->contact.warp ? scene_contact_light(scene, box3f_centre(r->world_bounds), drawn_scenes, drawn_scene_count)
                        : vec3f_unit_y();
    r->changed |= light.x != r->contact_light.x || light.y != r->contact_light.y || light.z != r->contact_light.z;
    r->contact_light = light;
    return scene_contact_bounds(scene, r, light);
}

static inline __attribute__((always_inline)) int
fill_renderer_changed(scene_t* scene, scene_renderer_t* r, int changes) {
    box3f_t current = r->world_bounds;
    if (r->contact.strength > 0.0F) {
        contact_blobs = true;
        current = box3f_union(current, fill_contact_bounds(scene, r));
    }
    if (r->changed && changes >= 0) {
        changes = changes < RENDER_PARTIAL_CHANGES_MAX ? changes : -1;
        if (changes >= 0) {
            changed[changes++] = box3f_union(r->drawn_bounds, current);
        }
    }
    r->changed = false;
    r->drawn_bounds = current;
    return changes;
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
        changes = fill_renderer_changed(scene, r, changes);
    }
    return changes;
}

/* Where each changed renderer was and is, for partial redraw; -1 when the
 * drawn set changed (a renderer left or joined) or more changed than fit.
 * Every drawn renderer's box is kept as where it was drawn. */
static __attribute__((noinline)) int
fill_changed(void) {
    int changes = 0;
    contact_blobs = false;
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

/* Not inlined: the log's arguments stay out of the draw's frame, which every
 * span below it stands on. */
static __attribute__((noinline)) void
log_refused(const scene_t* scene) {
    ESP_LOGE("scene", "no memory to draw '%s' (%u bytes free, largest block %u); it stays blank", scene->root->id,
             (unsigned)app_heap_free_bytes(), (unsigned)app_heap_largest_block());
}

void
scene_set_contact(entity_t renderer, const scene_contact_t* contact) {
    scene_row_t row;
    scene_t* scene = scene_entity_row(renderer, &row);
    if (scene == NULL) {
        return;
    }
    const int index = scene_renderer_index(scene, row);
    if (index < 0) {
        return;
    }
    scene_renderer_t* r = &scene->renderers[index];
    const bool refresh = r->contact.ground_y != contact->ground_y || r->contact.strength != contact->strength
                         || r->contact.shape != contact->shape;
    r->contact = *contact;
    /* Keep the silhouette buffers for reuse until scene_skin_give releases the skin. */
    if (refresh && contact->shape == SCENE_CONTACT_FEET) {
        scene_skin_contacts_find(scene, row);
    }
    r->changed = true;
    render_context_forget(render_context_main());
}

static void
post_draw(const r3d_span_target_t* target, const r3d_lens_t* lens, void* work, void* user) {
    (void)user;
    for (int k = 0; k < drawn_scene_count; k++) {
        scene_t* scene = drawn_scenes[k];
        for (int i = 0; i < scene->active_renderer_count; i++) {
            const scene_renderer_t* r = &scene->renderers[scene->active_renderers[i]];
            if (r->contact.strength > 0.0F && r->mesh.node_count > 0) {
                contact_blob_t single;
                contact_blob_t* footprints =
                    r->contact.shape == SCENE_CONTACT_FEET ? contact_blobs_work_footprints(work) : &single;
                const int count = scene_contact_blobs(scene, r, r->contact_light, footprints);
                contact_blobs_draw(target, lens, footprints, count, work);
            }
        }
    }
}

static void
draw_active(uint32_t dt_ms, int quarter, int width, int height) {
    scene_t* scene = active.scene;
    const scene_camera_t* camera = active_camera();
    list_drawn_scenes();
    for (int k = 0; k < drawn_scene_count; k++) {
        scene_resolve(drawn_scenes[k]);
    }
    /* Size measurements must use this draw's lens, including its viewport. */
    active_view(scene, camera, (viewport_t){width, height, quarter});
    if (fit_drawn()) {
        int batches = 0;
        const int count = fill_instances(&batches);
        if (count + batches == 0) {
            return;
        }
        render_context_set_changed(render_context_main(), changed, fill_changed());
        raster_t* raster = &render_context_main()->raster;
        static raster_post_t post = {.draw = post_draw};
        post.scratch_bytes = contact_blobs_work_bytes();
        raster->post = contact_blobs ? &post : NULL;
        raster->probes = scene_probes(scene->root->root);
        raster->light_transfer = scene->light_transfer;
        raster->tonemap_white = scene->tonemap_white;
        render_context_set_batches(render_context_main(), drawn.batch_draws, batches);
        rendered = render_context_draw(render_context_main(), drawn.instances, count, &drawn_view, camera->clear);
        if (rendered) {
            render_context_record_frame(render_context_main(), dt_ms);
        }
    }
    if (!rendered && !refused) {
        log_refused(scene);
        refused = true;
    }
}

static bool
render_frame(uint32_t dt_ms, int quarter, int width, int height, bool (*before_draw)(void)) {
    if (paused) {
        return before_draw == NULL;
    }
    stepped = true;
    for (int i = 0; i < SCENE_ROOTS_MAX; i++) {
        scene_t* scene = scene_ready_at(i);
        if (scene != NULL) {
            scene_animation_step(scene, dt_ms);
        }
    }
    if (before_draw != NULL && !before_draw()) {
        return false;
    }
    if (active.scene != NULL) {
        draw_active(dt_ms, quarter, width, height);
    }
    return true;
}

void
scene_render(uint32_t dt_ms, int quarter, int width, int height) {
    (void)render_frame(dt_ms, quarter, width, height, NULL);
}

#ifndef ESP_PLATFORM
bool
scene_render_with_setup(uint32_t dt_ms, int quarter, int width, int height, bool (*before_draw)(void)) {
    return render_frame(dt_ms, quarter, width, height, before_draw);
}
#endif

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
