/*
 * rt_cornell_scene - see rt_cornell_scene.h.
 *
 * float only, everywhere: the S3's FPU has no double, so a stray one falls
 * into software emulation an order of magnitude slower. Both pragmas below
 * make that a compile error rather than a hope.
 */
#pragma GCC diagnostic error "-Wdouble-promotion"
#pragma GCC diagnostic error "-Wfloat-conversion"

#include "rt_cornell_scene.h"

#define ROOM_HALF_X    1.0f
#define ROOM_HEIGHT    2.0f
#define ROOM_DEPTH     2.0f

#define LIGHT_HALF_X   0.24f
#define LIGHT_HALF_Z   0.24f
#define LIGHT_CENTER_Z 1.0f

#define WALL_WHITE     ((r3d_vec3f_t){0.76f, 0.75f, 0.74f})
#define WALL_RED       ((r3d_vec3f_t){0.63f, 0.065f, 0.05f})
#define WALL_GREEN     ((r3d_vec3f_t){0.14f, 0.45f, 0.091f})
#define BOX_ALBEDO     ((r3d_vec3f_t){0.78f, 0.78f, 0.75f})

/* sin/cos of 17 degrees, computed once here rather than by a trig call on
 * every ray/box test. */
#define BOX_YAW_SIN    0.29237170472f
#define BOX_YAW_COS    0.95630475596f

const r3d_vec3f_t rt_cornell_light_pos = {0.0f, ROOM_HEIGHT - 0.05f, LIGHT_CENTER_Z};

static const rt_wall_t light_quad = {
    {0.0f, -1.0f, 0.0f},           -ROOM_HEIGHT,       -LIGHT_HALF_X, LIGHT_HALF_X, LIGHT_CENTER_Z - LIGHT_HALF_Z,
    LIGHT_CENTER_Z + LIGHT_HALF_Z, {0.0f, 0.0f, 0.0f},
};

static const rt_wall_t walls[] = {
    {{0.0f, 1.0f, 0.0f}, 0.0f, -ROOM_HALF_X, ROOM_HALF_X, 0.0f, ROOM_DEPTH, WALL_WHITE},          /* floor */
    {{0.0f, -1.0f, 0.0f}, -ROOM_HEIGHT, -ROOM_HALF_X, ROOM_HALF_X, 0.0f, ROOM_DEPTH, WALL_WHITE}, /* ceiling */
    {{0.0f, 0.0f, -1.0f}, -ROOM_DEPTH, -ROOM_HALF_X, ROOM_HALF_X, 0.0f, ROOM_HEIGHT, WALL_WHITE}, /* back */
    {{1.0f, 0.0f, 0.0f}, -ROOM_HALF_X, 0.0f, ROOM_HEIGHT, 0.0f, ROOM_DEPTH, WALL_RED},            /* left */
    {{-1.0f, 0.0f, 0.0f}, -ROOM_HALF_X, 0.0f, ROOM_HEIGHT, 0.0f, ROOM_DEPTH, WALL_GREEN},         /* right */
};

static const rt_box_t boxes[] = {
    {{0.35f, 0.28f, 0.65f}, {0.28f, 0.28f, 0.28f}, -BOX_YAW_SIN, BOX_YAW_COS}, /* short, front-right */
    {{-0.35f, 0.55f, 1.3f}, {0.28f, 0.55f, 0.28f}, BOX_YAW_SIN, BOX_YAW_COS},  /* tall, back-left */
};

const rt_scene_t rt_cornell_scene = {
    .light = &light_quad,
    .walls = walls,
    .wall_count = (int)(sizeof(walls) / sizeof(walls[0])),
    .boxes = boxes,
    .box_count = (int)(sizeof(boxes) / sizeof(boxes[0])),
    .box_albedo = BOX_ALBEDO,
};
