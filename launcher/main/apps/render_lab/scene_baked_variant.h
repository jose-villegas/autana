/*
 * scene_baked_variant: the bakes every baked-scene family places, and the
 * renderer names they go by in its scene file: the family's stem and the
 * variant's suffix.
 */
#pragma once

typedef enum {
    SCENE_BAKED_FULL,
    SCENE_BAKED_FLAT,
    SCENE_BAKED_LITE,
    SCENE_BAKED_FITTED,
    SCENE_BAKED_FITTED_FULL,
    SCENE_BAKED_FLAT_FITTED,
    SCENE_BAKED_VARIANT_COUNT,
} scene_baked_variant_t;

/* The initialiser of a family's renderer names, each its stem and suffix. */
#define SCENE_BAKED_NAMES(stem_)                                                                                       \
    {[SCENE_BAKED_FULL] = stem_,                                                                                       \
     [SCENE_BAKED_FLAT] = stem_ "_flat",                                                                               \
     [SCENE_BAKED_LITE] = stem_ "_lite",                                                                               \
     [SCENE_BAKED_FITTED] = stem_ "_fitted",                                                                           \
     [SCENE_BAKED_FITTED_FULL] = stem_ "_fitted_full",                                                                 \
     [SCENE_BAKED_FLAT_FITTED] = stem_ "_flat_fitted"}
