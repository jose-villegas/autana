/*
 * vec3i: three plain integers, for a grid, pixel or cell coordinate. Its
 * float counterpart is vec3.h's vec3_t; a transform is float only.
 */
#pragma once

#include <stdint.h>

typedef struct {
    int32_t x, y, z;
} vec3i_t;
