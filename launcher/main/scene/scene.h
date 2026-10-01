/*
 * scene: the engine's scene manager. An app loads scenes by name and
 * activates one camera; the shell advances every loaded scene and draws
 * through the active camera each frame, so an app never calls draw.
 *
 * Storage is data-oriented. A scene is one allocation of packed component
 * arrays indexed by a small entity id: a transform per entity, a renderer
 * (mesh view plus entity id) and a camera (lens, path plus entity id) only
 * for the entities that have one. Names are looked up at setup time only.
 * Lights are baked offline and are not carried.
 *
 * Exactly one camera is active engine-wide, and it draws the enabled
 * renderers of its own scene. Scenes belong to the app running when they were
 * loaded: the shell unloads whatever is left when it exits.
 *
 * The table a scene is loaded from (scene_def_t) is generated const data,
 * registered by name with SCENE_REGISTER(); docs/render/Building-a-Scene.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "asset/asset_pack.h"
#include "render/r3d_instance.h"
#include "render/r3d_scene.h"
#include "render/raster.h"

typedef struct scene scene_t;

/* An index into one scene's component arrays. */
typedef uint16_t scene_entity_t;
#define SCENE_ENTITY_NONE ((scene_entity_t)0xFFFFU)

/* Where an entity stands: the placement the raster reads. */
typedef r3d_placement_t scene_transform_t;

typedef struct {
    scene_entity_t entity;
    const char* asset; /* the lit mesh's id in the asset pack */
} scene_renderer_def_t;

typedef struct {
    scene_entity_t entity;
    float half_fov_short_tan, near_z;
    const r3d_scene_path_t* path; /* NULL for a camera that stays put */
} scene_camera_def_t;

/* What scene_load() instantiates: array lengths and contents, all const. */
typedef struct {
    const char* name;
    uint16_t entity_count, renderer_count, camera_count;
    const char* const* entity_names;       /* entity_count */
    const scene_transform_t* transforms;   /* entity_count */
    const scene_renderer_def_t* renderers; /* renderer_count */
    const scene_camera_def_t* cameras;     /* camera_count */
} scene_def_t;

/* Why a load failed: the pack's status and the scene or asset id it was
 * about. */
typedef struct {
    asset_status_t status;
    const char* what;
} scene_failure_t;

/* Makes a def loadable by name; runs before main(), like APP_REGISTER(). */
void scene_register(const scene_def_t* def);
#define SCENE_REGISTER(def)                                                                                            \
    __attribute__((constructor)) static void def##_register(void) { scene_register(&(def)); }

/* Loads the scene `name` beside any already loaded, its meshes opened from
 * `pack` or the build's asset pack. NULL on failure; scene_load_failure()
 * says why. */
scene_t* scene_load(const char* name);
scene_t* scene_load_from(const asset_pack_t* pack, const char* name);
scene_failure_t scene_load_failure(void);

/* Frees the scene. If its camera was the active one, nothing is drawn. */
void scene_unload(scene_t* scene);

/* The entity named `name`, or SCENE_ENTITY_NONE. */
scene_entity_t scene_find(const scene_t* scene, const char* name);

const scene_transform_t* scene_entity_transform(const scene_t* scene, scene_entity_t entity);
void scene_entity_set_transform(scene_t* scene, scene_entity_t entity, const scene_transform_t* transform);
bool scene_entity_enabled(const scene_t* scene, scene_entity_t entity);

/* A disabled entity's renderer is not drawn; every entity starts enabled. */
void scene_entity_set_enabled(scene_t* scene, scene_entity_t entity, bool enabled);

/* Makes the scene's camera `camera` (NULL: its first) the active one. False
 * when the scene has no such camera. */
bool scene_activate(scene_t* scene, const char* camera);

/* No camera is active, so nothing is drawn, until the next scene_activate(). */
void scene_deactivate(void);

/* While paused nothing advances or draws, as if no camera were active, and
 * the camera stays where it was; for an app drawing its own full screen. */
void scene_set_paused(bool paused);

/* Settings of the active camera. The scale is the share of the destination's
 * size it renders at, upscaled on the way out. */
void scene_set_render_scale(int percent);
void scene_set_clear(uint32_t rgb);

/* What the last draw kept after culling. */
raster_stats_t scene_stats(void);

#if !defined(ESP_PLATFORM) || CONFIG_LAUNCHER_DEVELOPMENT
/* Between draw and upscale, shows the frame as `mode` says; development only. */
void scene_set_debug_view(raster_show_t mode);
#endif

/*
 * The shell's half. scene_render() advances the clocks and draws into the
 * raster's scratch block, touching no framebuffer, so it may overlap the last
 * frame's send. scene_compose() upscales into the target, drawing first if
 * scene_render() did not run, and so waits for the framebuffer to be free.
 */
typedef struct {
    uint16_t* pixels;
    int width, height;
} scene_target_t;

bool scene_has_active_camera(void);
void scene_render(uint32_t dt_ms, int quarter, const scene_target_t* target);
void scene_compose(uint32_t dt_ms, int quarter, const scene_target_t* target);

/* The two above on the panel's framebuffer and orientation, marking it dirty. */
void scene_shell_render(uint32_t dt_ms);
void scene_shell_compose(uint32_t dt_ms);

/* An app has exited: unloads every scene and frees the raster's scratch. */
void scene_unload_all(void);
