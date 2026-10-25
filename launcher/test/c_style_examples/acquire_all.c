/* Return both buffers to the caller, or release every partial acquisition. */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

static bool
acquire_all(size_t n, uint8_t** out_a, uint8_t** out_b) {
    uint8_t* a = malloc(n);
    uint8_t* b = malloc(n);
    if (a == NULL || b == NULL) {
        free(a); /* free(NULL) is a no-op */
        free(b);
        return false;
    }
    *out_a = a;
    *out_b = b;
    return true;
}
