/*
 * recti: a half-open rectangle of an int. The operations are
 * rect_template.h's; docs/math/README.md says how they are made.
 */
#pragma once

#include "math/geometry/rect_template.h"
#include "math/scalar/mathi.h"

MATH_DEFINE_RECT(recti, int, mathi)
