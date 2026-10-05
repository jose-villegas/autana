#pragma once

/* Force-included into every main-component source of a seeded build
 * (LAUNCHER_LAYOUT_SEED, launcher/main/CMakeLists.txt). Each pad is a retained
 * section of its own, so --gc-sections keeps it and the linker places it
 * ahead of the object's other code or rodata. Nothing refers to it. */
#define LAYOUT_PAD_STR2(x) #x
#define LAYOUT_PAD_STR(x)  LAYOUT_PAD_STR2(x)
#define LAYOUT_PAD_SECTION(name, flags, align, bytes)                                                                  \
    ".pushsection " name ",\"" flags                                                                                   \
    "\",@progbits\n.balign " LAYOUT_PAD_STR(align) "\n.space " LAYOUT_PAD_STR(bytes) "\n.popsection\n"

#if LAYOUT_PAD_TEXT > 0
__asm__(LAYOUT_PAD_SECTION(".text.layout_pad", "axR", LAYOUT_PAD_TEXT_ALIGN, LAYOUT_PAD_TEXT));
#endif

#if LAYOUT_PAD_RODATA > 0
__asm__(LAYOUT_PAD_SECTION(".rodata.layout_pad", "aR", LAYOUT_PAD_RODATA_ALIGN, LAYOUT_PAD_RODATA));
#endif
