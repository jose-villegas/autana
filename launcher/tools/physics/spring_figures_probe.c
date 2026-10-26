/* spring_figures_probe: a spring line's wave, tick by tick, for the spring reference. */
#include <stdio.h>
#include <stdlib.h>

#include "physics/spring/spring_line.h"

static const int line_columns = 41;
static const int line_ticks = 252;
static const int bump_half_width = 4;
static const int bump_pixels = 20;
static const int tension = 180;
static const int stiffness = 2;
static const int damping = 12;

int
main(void) {
    int32_t* offset = calloc((size_t)line_columns, sizeof *offset);
    int32_t* velocity = calloc((size_t)line_columns, sizeof *velocity);
    if (!offset || !velocity) {
        free(offset);
        free(velocity);
        return EXIT_FAILURE;
    }
    spring_line_t line;
    spring_line_init(&line, offset, velocity, line_columns, tension, stiffness, damping);
    spring_line_nudge(&line, line_columns / 2, bump_half_width, bump_pixels * MATHX_ONE);
    printf("{\"tick_ms\":%d,\"samples\":[", SPRING_LINE_TICK_MS);
    for (int tick = 0; tick <= line_ticks; tick++) {
        printf("%s{\"tick\":%d,\"lo\":%d,\"hi\":%d,\"rest\":%s,\"offset\":[", tick ? "," : "", tick, line.active_lo,
               line.active_hi, spring_line_at_rest(&line) ? "true" : "false");
        for (int x = 0; x < line_columns; x++) {
            printf("%s%.9g", x ? "," : "", (double)offset[x] / MATHX_ONE);
        }
        printf("]}");
        spring_line_advance(&line, SPRING_LINE_TICK_MS);
    }
    puts("]}");
    free(offset);
    free(velocity);
    return 0;
}
