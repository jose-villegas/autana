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

touch_probe_target_t
touch_probe_grid(int index, int cols, int rows, int screen_w, int screen_h, int side, int margin) {
    const int span_x = screen_w - 2 * margin - side;
    const int span_y = screen_h - 2 * margin - side;
    const int col = index % cols;
    const int row = index / cols;
    const touch_probe_target_t t = {
        .x = margin + (cols > 1 ? span_x * col / (cols - 1) : span_x / 2),
        .y = margin + (rows > 1 ? span_y * row / (rows - 1) : span_y / 2),
        .side = side,
    };
    return t;
}

void
touch_probe_shuffle(uint32_t* rng, int* order, int n) {
    for (int i = 0; i < n; i++) {
        order[i] = i;
    }
    for (int i = n - 1; i > 0; i--) {
        const int j = random_in(rng, 0, i);
        const int swap = order[i];
        order[i] = order[j];
        order[j] = swap;
    }
}

#define SETTLE_MAX 64

static int
median(int* v, int n) {
    for (int i = 1; i < n; i++) {
        const int key = v[i];
        int j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }
    return v[n / 2];
}

void
touch_probe_settled(const touch_probe_sample_t* samples, int n, int from_ms, int to_ms, int* x, int* y) {
    int xs[SETTLE_MAX], ys[SETTLE_MAX];
    int count = 0;
    int last = 0;
    for (int i = 0; i < n; i++) {
        if (samples[i].t_ms <= to_ms) {
            last = i;
        }
        if (samples[i].t_ms >= from_ms && samples[i].t_ms <= to_ms && count < SETTLE_MAX) {
            xs[count] = samples[i].x;
            ys[count] = samples[i].y;
            count++;
        }
    }
    if (count == 0) {
        *x = samples[last].x;
        *y = samples[last].y;
        return;
    }
    *x = median(xs, count);
    *y = median(ys, count);
}
