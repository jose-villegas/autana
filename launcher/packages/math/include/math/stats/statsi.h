/* statsi: order statistics of int32 samples, such as timings in microseconds or cycles. */
#pragma once
#include <stdint.h>

#include "math/stats/stats_template.h"

MATH_DEFINE_STATS(statsi, int32_t)
