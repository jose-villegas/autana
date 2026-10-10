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
#define LAYOUT_PAD_STR2(x) #x
#define LAYOUT_PAD_STR(x)  LAYOUT_PAD_STR2(x)
#define LAYOUT_PAD_SECTION(name, flags, align, body)                                                                   \
    ".pushsection " name ",\"" flags "\",@progbits\n.balign " LAYOUT_PAD_STR(align) "\n" body ".popsection\n"

/* An L32R is three bytes; the pad is a whole number of cache lines. */
#define LAYOUT_PAD_L32R_BYTES 3

#if LAYOUT_PAD_TEXT > 0
__asm__(LAYOUT_PAD_SECTION(".text.layout_pad", "axR", LAYOUT_PAD_TEXT_ALIGN,
                           ".literal .Llayout_pad_literal, 0\nl32r a2, .Llayout_pad_literal\n.space " LAYOUT_PAD_STR(
                               LAYOUT_PAD_TEXT) " - " LAYOUT_PAD_STR(LAYOUT_PAD_L32R_BYTES) "\n"));
#endif

#if LAYOUT_PAD_RODATA > 0
__asm__(LAYOUT_PAD_SECTION(".rodata.layout_pad", "aR", LAYOUT_PAD_RODATA_ALIGN,
                           ".space " LAYOUT_PAD_STR(LAYOUT_PAD_RODATA) "\n"));
#endif
