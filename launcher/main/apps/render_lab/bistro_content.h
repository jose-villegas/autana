/*
 * bistro_content: the Bistro scene's id, its bakes and its camera. An open
 * street with a far plane: a loop walks the street and flies over the roofs.
 * Model units are centimetres; y is up.
 */
#pragma once

/* The scene's id, and its pack's name. */
#define BISTRO_SCENE "bistro"

typedef enum {
    BISTRO_BAKE_FULL,
    BISTRO_BAKE_FLAT,
    BISTRO_BAKE_LITE,
    BISTRO_BAKE_FITTED,
    BISTRO_BAKE_FITTED_FULL,
    BISTRO_BAKE_FLAT_FITTED,
    BISTRO_BAKE_COUNT,
} bistro_bake_t;

/* The name of the entity that draws each bake, found at load. */
extern const char* const bistro_bakes[BISTRO_BAKE_COUNT];

#define BISTRO_CAMERA_COUNT 1
extern const char* const bistro_cameras[BISTRO_CAMERA_COUNT];
