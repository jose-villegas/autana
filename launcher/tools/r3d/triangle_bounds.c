/* triangle_bounds: enumerate the triangles whose bounds can meet each point's search radius. */
#include <stddef.h>
#include <stdint.h>

#define AXES           3
#define CHILDREN       2
#define STACK_CAPACITY 64

typedef struct {
    const double* low;
    const double* high;
    const int64_t* children;
    const int64_t* start;
    const int64_t* count;
    const int64_t* order;
    const double* triangle_low;
    const double* triangle_high;
} bounds_index_t;

static double
box_distance_squared(const double* point, const double* low, const double* high) {
    double squared = 0.0;
    for (int axis = 0; axis < AXES; axis++) {
        const double delta = point[axis] < low[axis]    ? low[axis] - point[axis]
                             : point[axis] > high[axis] ? point[axis] - high[axis]
                                                        : 0.0;
        squared += delta * delta;
    }
    return squared;
}

size_t
triangle_bound_candidates(const bounds_index_t* index, const double* points, const double* reach_squared,
                          size_t point_count, int64_t* owners, int64_t* triangles, size_t capacity) {
    size_t written = 0;
    for (size_t i = 0; i < point_count; i++) {
        const double* point = points + i * AXES;
        int64_t stack[STACK_CAPACITY] = {0};
        size_t depth = 1;
        while (depth != 0) {
            const int64_t node = stack[--depth];
            if (box_distance_squared(point, index->low + node * AXES, index->high + node * AXES) > reach_squared[i]) {
                continue;
            }
            if (index->count[node] == 0) {
                if (depth + CHILDREN > STACK_CAPACITY) {
                    return SIZE_MAX;
                }
                for (int child = 0; child < CHILDREN; child++) {
                    stack[depth++] = index->children[node * CHILDREN + child];
                }
                continue;
            }
            const int64_t stop = index->start[node] + index->count[node];
            for (int64_t j = index->start[node]; j < stop; j++) {
                const int64_t triangle = index->order[j];
                const double* low = index->triangle_low + triangle * AXES;
                const double* high = index->triangle_high + triangle * AXES;
                if (box_distance_squared(point, low, high) > reach_squared[i]) {
                    continue;
                }
                if (written == capacity) {
                    return SIZE_MAX;
                }
                owners[written] = (int64_t)i;
                triangles[written++] = triangle;
            }
        }
    }
    return written;
}
