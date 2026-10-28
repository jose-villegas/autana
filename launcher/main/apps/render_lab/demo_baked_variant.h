/*
 * demo_baked_variant: the bakes every baked-scene family places, each a
 * scene in render_lab named after its family's name and key with the title
 * and key suffixes, and its menu label. The renderers that draw them are named
 * by each family's scene file (baked_families.c).
 */
#pragma once

/* X(context, id, title suffix, key suffix, menu label), one row per variant:
 * the one place a variant is spelled out. `context` is what the caller passes
 * after X, handed to every row. */
#define DEMO_BAKED_VARIANTS(X, context)                                                                                \
    X(context, FULL, "", "", "Full")                                                                                   \
    X(context, FLAT, " Flat", "-flat", "Flat")                                                                         \
    X(context, LITE, " Lite", "-lite", "Lite")                                                                         \
    X(context, FITTED, " Fitted", "-fitted", "Fitted")                                                                 \
    X(context, FITTED_FULL, " Fitted Full", "-fitted-full", "Fitted Full")                                             \
    X(context, FLAT_FITTED, " Flat Fitted", "-flat-fitted", "Flat Fitted")

#define DEMO_BAKED_VARIANT_ID(context_, id_, title_, key_, label_) DEMO_BAKED_##id_,

typedef enum {
    DEMO_BAKED_VARIANTS(DEMO_BAKED_VARIANT_ID, ) DEMO_BAKED_VARIANT_COUNT,
} demo_baked_variant_t;
