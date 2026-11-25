/* test_scene_timeline: shares the scene suite's SCNE and rig fixture writers. */
#pragma once

#include <stdbool.h>

#include "test_pack.h"

#define SCENE_TEST_TIMELINE_ID "timeline"
#define SCENE_TEST_STATIC_ID   "static_scene"

enum {
    VIS_RED,
    VIS_GREEN,
    VIS_BLUE,
    VIS_CAMERA,
    VIS_LAMP,
    VIS_PARENT,
    VIS_CHILD,
    VIS_ROWS,
    VIS_COPIES = VIS_BLUE + 1,
};

uint8_t* scene_test_timeline_entry(test_pack_t* pack, uint8_t* bytes, size_t capacity, const uint16_t* counts,
                                   const size_t* sizes, bool property);
