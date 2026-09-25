#include "apps/input_lab/touch_probe.h"

#include <math.h>

static uint32_t
xorshift32(uint32_t* state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

static int
random_in(uint32_t* rng, int lo, int hi) {
    return lo + (int)(xorshift32(rng) % (uint32_t)(hi - lo + 1));
}

touch_probe_target_t
touch_probe_next(uint32_t* rng, int screen_w, int screen_h, int side, int margin) {
    const touch_probe_target_t t = {
        .x = random_in(rng, margin, screen_w - margin - side),
        .y = random_in(rng, margin, screen_h - margin - side),
        .side = side,
    };
    return t;
}

void
touch_probe_offset(touch_probe_target_t target, int x, int y, int* dx, int* dy) {
    *dx = x - (target.x + target.side / 2);
    *dy = y - (target.y + target.side / 2);
}

bool
touch_probe_record(touch_probe_stats_t* stats, touch_probe_target_t target, int x, int y) {
    int dx, dy;
    touch_probe_offset(target, x, y, &dx, &dy);
    const bool hit = x >= target.x && x < target.x + target.side && y >= target.y && y < target.y + target.side;

    stats->taps++;
    stats->hits += hit;
    stats->sum_dx += dx;
    stats->sum_dy += dy;
    stats->sum_dx2 += (int64_t)dx * dx;
    stats->sum_dy2 += (int64_t)dy * dy;
    if (dx * dx + dy * dy > stats->worst_dist2) {
        stats->worst_dist2 = dx * dx + dy * dy;
    }
    return hit;
}

static float
mean(int64_t sum, int n) {
    return n > 0 ? (float)sum / (float)n : 0.0f;
}

static float
spread(int64_t sum, int64_t sum2, int n) {
    if (n < 2) {
        return 0.0f;
    }
    const float m = (float)sum / (float)n;
    const float variance = ((float)sum2 - (float)n * m * m) / (float)(n - 1);
    return variance > 0.0f ? sqrtf(variance) : 0.0f;
}

float
touch_probe_mean_dx(const touch_probe_stats_t* stats) {
    return mean(stats->sum_dx, stats->taps);
}

float
touch_probe_mean_dy(const touch_probe_stats_t* stats) {
    return mean(stats->sum_dy, stats->taps);
}

float
touch_probe_spread_dx(const touch_probe_stats_t* stats) {
    return spread(stats->sum_dx, stats->sum_dx2, stats->taps);
}

float
touch_probe_spread_dy(const touch_probe_stats_t* stats) {
    return spread(stats->sum_dy, stats->sum_dy2, stats->taps);
}
