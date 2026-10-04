#pragma once

/* Force-included into every main-component source of a seeded build
 * (LAUNCHER_LAYOUT_SEED, launcher/main/CMakeLists.txt). Each pad is a retained
 * section of its own, so --gc-sections keeps it and the linker places it
 * ahead of the object's other code or rodata. Nothing refers to it. */
#define LAYOUT_PAD_STR2(x) #x
#define LAYOUT_PAD_STR(x)  LAYOUT_PAD_STR2(x)

#if LAYOUT_PAD_TEXT > 0
__asm__(".pushsection .text.layout_pad,\"axR\",@progbits\n"
        ".balign 32\n"
        ".space " LAYOUT_PAD_STR(LAYOUT_PAD_TEXT) "\n"
                                                  ".popsection\n");
#endif

#if LAYOUT_PAD_RODATA > 0
__asm__(".pushsection .rodata.layout_pad,\"aR\",@progbits\n"
        ".balign 64\n"
        ".space " LAYOUT_PAD_STR(LAYOUT_PAD_RODATA) "\n"
                                                    ".popsection\n");
#endif
