/* repeat: the reference and the call that hands aligned blocks to PIE. */
#include "simd/repeat.h"

#include "simd/simd.h"

#if SIMD_PIE
#include "pie/pie_kernels.h"
#endif

void
simd_repeat2_u16_ref(uint16_t* dst, const uint16_t* src, size_t count) {
    for (size_t i = 0; i < count; i++) {
        dst[2 * i] = src[i];
        dst[(2 * i) + 1] = src[i];
    }
}

void
simd_repeat2_u16(uint16_t* dst, const uint16_t* src, size_t count) {
#if SIMD_PIE
    if (count >= SIMD_REPEAT2_U16_MIN) {
        /* Each source element is two output elements, so dst reaches its
         * boundary after half as many source elements. */
        const size_t head = simd_head_bytes(dst) / (2 * sizeof *dst);
        if (simd_head_bytes(dst) % (2 * sizeof *dst) == 0) {
            simd_repeat2_u16_ref(dst, src, head);
            dst += 2 * head;
            src += head;
            count -= head;
            const size_t blocks = count / SIMD_LANES_U16;
            if (simd_is_aligned(src)) {
                simd_pie_repeat2_u16(dst, src, blocks);
            } else {
                simd_pie_repeat2_u16_unaligned(dst, src, blocks);
            }
            dst += 2 * blocks * SIMD_LANES_U16;
            src += blocks * SIMD_LANES_U16;
            count -= blocks * SIMD_LANES_U16;
        }
    }
#endif
    simd_repeat2_u16_ref(dst, src, count);
}
