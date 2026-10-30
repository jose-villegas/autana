/* fill: the reference and the call that hands aligned blocks to PIE. */
#include "simd/fill.h"

#include "simd/simd.h"

#if SIMD_PIE
#include "pie/pie_kernels.h"
#endif

void
simd_fill_u16_ref(uint16_t* dst, uint16_t value, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[i] = value;
    }
}

void
simd_fill_u16(uint16_t* dst, uint16_t value, size_t count) {
#if SIMD_PIE
    if (count >= SIMD_FILL_U16_MIN) {
        const size_t head = simd_head_bytes(dst) / sizeof *dst;
        simd_fill_u16_ref(dst, value, head);
        dst += head;
        count -= head;
        const size_t blocks = count / SIMD_LANES_U16;
        simd_pie_fill_u16(dst, SIMD_U16_PAIR(value), blocks);
        dst += blocks * SIMD_LANES_U16;
        count -= blocks * SIMD_LANES_U16;
    }
#endif
    simd_fill_u16_ref(dst, value, count);
}
