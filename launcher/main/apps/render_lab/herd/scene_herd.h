/*
 * scene_herd: the Herd family, keyed "herd-" and its size: N skinned
 * capybaras walking a meadow, keeping apart, each playing the clip and rate
 * its ground speed calls for, while one finger orbits the camera round the
 * area they walk. The pack holds a pool of copies on rings out from the
 * centre, where they start; a size shows the first N and hides the rest.
 */
#pragma once

#include <stdbool.h>

#include "lab/lab_scene.h"

/* X(ID, key, copies): each size, its key the scene key's suffix and its menu label. */
#define HERD_SIZES(X)                                                                                                  \
    X(ONE, "1", 1)                                                                                                     \
    X(FOUR, "4", 4)                                                                                                    \
    X(EIGHT, "8", 8)

#define HERD_SIZE_ID(id_, ...) HERD_##id_,

typedef enum { HERD_SIZES(HERD_SIZE_ID) HERD_SIZE_COUNT } herd_size_t;

extern const lab_scene_t scene_herd[HERD_SIZE_COUNT];

/* Whether the next enter holds the copies still where the pack places them,
 * each in its own clip at its own phase, and fits the orbit to them: for a
 * test that places them itself. Off until set. */
void scene_herd_hold(bool hold);
