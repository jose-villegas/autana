/*
 * pie_asm: the frame every PIE kernel (a .S file under src/pie/) opens and
 * closes, so no kernel picks its own stack offsets. Its macros exist for
 * assembly only; a C file that includes it sees SIMD_PIE_FRAME_BYTES alone.
 *
 * Under the windowed ABI the top 16 bytes of a frame opened by `entry a1, N`
 * are where a window overflow spills the caller's a0..a3: a kernel that
 * stores there corrupts its caller's return. The kernels keep everything in
 * registers and store nothing to their frame.
 *
 * The kernels are leaves: their LOOPGTZ owns LBEG/LEND/LCOUNT, which no
 * caller keeps live across a call. No kernel may run in an ISR (coprocessor
 * state is thread-only), and the first one a task runs pins it to its core.
 */
#pragma once

/* Bytes `entry` reserves: the smallest windowed frame, whose top 16 bytes
 * are the spill area. */
#define SIMD_PIE_FRAME_BYTES 32

#ifdef __ASSEMBLER__
/* clang-format off */
.macro SIMD_PIE_FUNC name
    .section .text.\name, "ax"
    .global \name
    .type \name, @function
    .align 4
\name:
    entry a1, SIMD_PIE_FRAME_BYTES
.endm

.macro SIMD_PIE_END name
    retw.n
    .size \name, . - \name
.endm
/* clang-format on */
#endif
