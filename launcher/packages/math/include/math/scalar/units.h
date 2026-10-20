/*
 * units: integer time conversion scales.
 */
#pragma once

#define MATH_MS_PER_S   1000u

#define MATH_US_PER_MS  1000u
#define MATH_US_PER_S   (MATH_US_PER_MS * MATH_MS_PER_S)

#define MATH_S_PER_MIN  60u
#define MATH_MS_PER_MIN (MATH_S_PER_MIN * MATH_MS_PER_S)
