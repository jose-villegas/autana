/* anim_tracks_format: the TRCK file layout stored in an asset pack. */
#pragma once

#include "asset/asset_pack.h"

#define ANIM_TRACKS_ASSET           ASSET_TYPE('T', 'R', 'C', 'K')
#define ANIM_TRACKS_VERSION         2U
#define ANIM_COMPONENT_TRANSFORM    ASSET_TYPE('T', 'R', 'N', 'S')
#define ANIM_BINDING_STRING_MAX     255U

#define ANIM_ROOT_SCENE             0
#define ANIM_ROOT_SKELETON          1

#define ANIM_TRACKS_HEADER_SIZE     20
#define ANIM_TRACKS_AT_VERSION      0
#define ANIM_TRACKS_AT_COUNT        2
#define ANIM_TRACKS_AT_DURATION     4
#define ANIM_TRACKS_AT_STRINGS      8
#define ANIM_TRACKS_AT_STRINGS_SIZE 12
#define ANIM_TRACKS_AT_ROOT         16
#define ANIM_TRACKS_AT_PAD          17
#define ANIM_TRACKS_HEADER_PAD_SIZE 3

#define ANIM_TRACKS_ROW_SIZE        24
#define ANIM_TRACKS_ROW_PATH        0
#define ANIM_TRACKS_ROW_FIELD       2
#define ANIM_TRACKS_ROW_COMPONENT   4
#define ANIM_TRACKS_ROW_TIMES       8
#define ANIM_TRACKS_ROW_VALUES      12
#define ANIM_TRACKS_ROW_KEYS        16
#define ANIM_TRACKS_ROW_TYPE        18
#define ANIM_TRACKS_ROW_INTERP      19
#define ANIM_TRACKS_ROW_PAD         20
#define ANIM_TRACKS_ROW_PAD_SIZE    4

#define ANIM_TRACKS_ALIGNMENT       4
#define ANIM_TRACKS_CUBIC_RUNS      3
