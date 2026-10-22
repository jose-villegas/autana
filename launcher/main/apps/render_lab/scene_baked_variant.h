/*
 * scene_baked_variant: the bakes every baked-scene family places. Each variant
 * names its renderer in the family's scene file (the family's stem and the
 * renderer suffix) and its scene in render_lab (the family's name and key with
 * the title and key suffixes, and its menu label).
 */
#pragma once

/* X(context, id, renderer suffix, title suffix, key suffix, menu label), one
 * row per variant: the one place a variant is spelled out. `context` is what
 * the caller passes after X, handed to every row. */
#define SCENE_BAKED_VARIANTS(X, context)                                                                               \
    X(context, FULL, "", "", "", "Full")                                                                               \
    X(context, FLAT, "_flat", " Flat", "-flat", "Flat")                                                                \
    X(context, LITE, "_lite", " Lite", "-lite", "Lite")                                                                \
    X(context, FITTED, "_fitted", " Fitted", "-fitted", "Fitted")                                                      \
    X(context, FITTED_FULL, "_fitted_full", " Fitted Full", "-fitted-full", "Fitted Full")                             \
    X(context, FLAT_FITTED, "_flat_fitted", " Flat Fitted", "-flat-fitted", "Flat Fitted")

#define SCENE_BAKED_VARIANT_ID(context_, id_, renderer_, title_, key_, label_) SCENE_BAKED_##id_,

typedef enum {
    SCENE_BAKED_VARIANTS(SCENE_BAKED_VARIANT_ID, ) SCENE_BAKED_VARIANT_COUNT,
} scene_baked_variant_t;

/* The initialiser of a family's renderer names, each its stem and suffix. */
#define SCENE_BAKED_NAME(stem_, id_, renderer_, title_, key_, label_) [SCENE_BAKED_##id_] = stem_ renderer_,
#define SCENE_BAKED_NAMES(stem_)                                      {SCENE_BAKED_VARIANTS(SCENE_BAKED_NAME, stem_)}
