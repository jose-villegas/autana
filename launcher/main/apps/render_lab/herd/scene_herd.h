/*
 * scene_herd: the Herd family, keyed "herd-" and its size: N skinned
 * capybaras on a meadow, each playing its own clip at its own phase, while
 * one finger orbits the camera round the copies shown. The pack holds a pool
 * of copies on rings out from the centre; a size shows the first N and hides
 * the rest, so zoomed in, copies fall off the frame and behind the camera.
 */
#pragma once

#include "lab/lab_scene.h"

/* X(ID, key, copies): each size, its key the scene key's suffix and its menu label. */
#define HERD_SIZES(X)                                                                                                  \
    X(ONE, "1", 1)                                                                                                     \
    X(FOUR, "4", 4)                                                                                                    \
    X(EIGHT, "8", 8)

#define HERD_SIZE_ID(id_, ...) HERD_##id_,

typedef enum { HERD_SIZES(HERD_SIZE_ID) HERD_SIZE_COUNT } herd_size_t;

extern const lab_scene_t scene_herd[HERD_SIZE_COUNT];
