#pragma once

/* Force-included into every main-component source of a seeded build
 * (LAUNCHER_LAYOUT_SEED, launcher/main/CMakeLists.txt). Each pad is a section
 * of its own ahead of the object's other code or rodata, kept through its
 * symbol (-u layout_pad_text_<id>). The code pad loads one literal: within a
 * linker rule the Xtensa linker places the sections that load none after all
 * the rest, past the code a pad should move. Nothing calls it. */
#define LAYOUT_PAD_STR2(x) #x
#define LAYOUT_PAD_STR(x)  LAYOUT_PAD_STR2(x)
#define LAYOUT_PAD_SECTION(name, flags, align, fill, symbol)                                                           \
    ".pushsection " name ",\"" flags "\",@progbits\n.balign " LAYOUT_PAD_STR(align) "\n.globl " symbol "\n" symbol     \
                                                                                    ":\n" fill "\n.popsection\n"

#if LAYOUT_PAD_TEXT > 0
__asm__(LAYOUT_PAD_SECTION(".text.layout_pad_" LAYOUT_PAD_STR(LAYOUT_PAD_ID), "ax", LAYOUT_PAD_TEXT_ALIGN,
                           ".literal .Llayout_pad, 0\nl32r a2, .Llayout_pad\nnop\n.rept (" LAYOUT_PAD_STR(
                               LAYOUT_PAD_TEXT) " - 6) / 2\nnop.n\n.endr",
                           "layout_pad_text_" LAYOUT_PAD_STR(LAYOUT_PAD_ID)));
#endif

#if LAYOUT_PAD_RODATA > 0
__asm__(LAYOUT_PAD_SECTION(".rodata.layout_pad", "a", LAYOUT_PAD_RODATA_ALIGN,
                           ".space " LAYOUT_PAD_STR(LAYOUT_PAD_RODATA),
                           "layout_pad_rodata_" LAYOUT_PAD_STR(LAYOUT_PAD_ID)));
#endif
