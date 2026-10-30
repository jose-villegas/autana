/* copy: the long-run path that hands aligned steps to PIE. */
#include "simd/copy.h"

#if SIMD_PIE
#include "pie/pie_kernels.h"
#endif

void
simd_copy_long(void* dst, const void* src, size_t bytes) {
#if SIMD_PIE
    uint8_t* d = dst;
    const uint8_t* s = src;
    const size_t head = simd_head_bytes(d);
    memcpy(d, s, head);
    d += head;
    s += head;
    bytes -= head;
    const size_t step = 2 * SIMD_ALIGN;
    const size_t steps = bytes / step;
    if (simd_is_aligned(s)) {
        simd_pie_copy(d, s, steps);
    } else {
        simd_pie_copy_unaligned(d, s, steps);
    }
    dst = d + (steps * step);
    src = s + (steps * step);
    bytes -= steps * step;
#endif
    simd_copy_ref(dst, src, bytes);
}
