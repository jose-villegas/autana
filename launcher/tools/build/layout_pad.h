#pragma once

/* Force-included into every main-component source of a seeded build
 * (LAUNCHER_LAYOUT_SEED, launcher/main/CMakeLists.txt). Each pad is a retained
 * section of its own, so --gc-sections keeps it and the linker places it
 * ahead of the object's other code or rodata. Nothing refers to it.
 *
 * The code pad opens with one L32R of a literal of its own: the Xtensa linker
 * moves code that loads no literal behind all other code of its link rule,
 * where a pad would shift nothing. layout_pad.py --check proves the placement
 * after every seeded link. */

/* Each pad's directives, written once: an assembly source takes them as
 * they are, a C source as a string. `;` ends a statement, for gas as for a
 * line break. */
#define LAYOUT_PAD_STR2(...)  #__VA_ARGS__
#define LAYOUT_PAD_STR(...)   LAYOUT_PAD_STR2(__VA_ARGS__)

/* An L32R is three bytes; the pad is a whole number of cache lines. */
#define LAYOUT_PAD_L32R_BYTES 3

/* clang-format off */
#define LAYOUT_PAD_TEXT_DIRECTIVES                                                                                         .pushsection .text.layout_pad, "axR", @progbits; .balign LAYOUT_PAD_TEXT_ALIGN;                                        .literal .Llayout_pad_literal, 0; l32r a2, .Llayout_pad_literal;                                                       .space LAYOUT_PAD_TEXT - LAYOUT_PAD_L32R_BYTES; .popsection
#define LAYOUT_PAD_RODATA_DIRECTIVES                                                                                       .pushsection .rodata.layout_pad, "aR", @progbits; .balign LAYOUT_PAD_RODATA_ALIGN;                                     .space LAYOUT_PAD_RODATA; .popsection
/* clang-format on */

#ifdef __ASSEMBLER__
#define LAYOUT_PAD_EMIT(directives) directives
#else
#define LAYOUT_PAD_EMIT(directives) __asm__(LAYOUT_PAD_STR(directives))
#endif

#if LAYOUT_PAD_TEXT > 0
LAYOUT_PAD_EMIT(LAYOUT_PAD_TEXT_DIRECTIVES);
#endif
#if LAYOUT_PAD_RODATA > 0
LAYOUT_PAD_EMIT(LAYOUT_PAD_RODATA_DIRECTIVES);
#endif
