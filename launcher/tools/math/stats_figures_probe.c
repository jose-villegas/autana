/* stats_figures_probe: the library's percentiles and median of two frame-time samples, one of odd and one of even length. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "math/stats/statsi.h"

/* Frame times in milliseconds, distinct so each selected value names one rank. */
static const int32_t odd_sample[] = {14, 9, 21, 12, 30, 11, 16, 13, 10, 18, 25};
static const int32_t even_sample[] = {14, 9, 21, 12, 30, 11, 16, 13, 10, 18};
static const unsigned percents[] = {0, 25, 50, 75, 95, 100};

enum { SAMPLE_MAX = 16 };

static void
sample(const char* name, const int32_t* values, size_t count, int first) {
    int32_t work[SAMPLE_MAX];
    printf("%s\"%s\":{\"values\":[", first ? "" : ",", name);
    for (size_t i = 0; i < count; i++) {
        printf("%s%ld", i ? "," : "", (long)values[i]);
    }
    printf("],\"percentiles\":[");
    for (size_t p = 0; p < sizeof percents / sizeof percents[0]; p++) {
        memcpy(work, values, count * sizeof *values);
        printf("%s[%u,%ld]", p ? "," : "", percents[p], (long)statsi_percentile(work, count, percents[p]));
    }
    memcpy(work, values, count * sizeof *values);
    printf("],\"median\":%ld}", (long)statsi_median(work, count));
}

int
main(void) {
    _Static_assert(sizeof odd_sample / sizeof odd_sample[0] <= SAMPLE_MAX, "odd sample fits the work buffer");
    _Static_assert(sizeof even_sample / sizeof even_sample[0] <= SAMPLE_MAX, "even sample fits the work buffer");
    printf("{");
    sample("odd", odd_sample, sizeof odd_sample / sizeof odd_sample[0], 1);
    sample("even", even_sample, sizeof even_sample / sizeof even_sample[0], 0);
    printf("}\n");
    return 0;
}
