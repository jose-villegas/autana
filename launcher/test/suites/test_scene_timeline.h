/* test_scene_timeline: shares the scene suite's SCNE and rig fixture writers. */
#pragma once

#include <stdbool.h>

#include "test_pack.h"

#define SCENE_TEST_TIMELINE_ID "timeline"

uint8_t* scene_test_timeline_entry(test_pack_t* pack, uint8_t* bytes, size_t capacity, const uint16_t* counts,
                                   const size_t* sizes, bool property);
