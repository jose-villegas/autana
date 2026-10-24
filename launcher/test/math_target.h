/* Build identity for math measurement captures. */
#pragma once

#include <stdio.h>

#ifdef DEVICE_BUILD
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include <xtensa/config/core-isa.h>
#endif

static inline void
math_target_report(void) {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    const char* arch = "Xtensa";
#elif CONFIG_IDF_TARGET_ARCH_RISCV
    const char* arch = "RISC-V";
#else
    const char* arch = "unknown";
#endif
#if CONFIG_LAUNCHER_QEMU
    const char* execution = "qemu";
#else
    const char* execution = "hardware";
#endif
#ifdef XCHAL_CORE_ID
    printf("MATHTARGET target=%s arch=%s core=%s execution=%s\n", CONFIG_IDF_TARGET, arch, XCHAL_CORE_ID, execution);
#else
    printf("MATHTARGET target=%s arch=%s execution=%s\n", CONFIG_IDF_TARGET, arch, execution);
#endif
}
#else
#define math_target_report() ((void)0)
#endif
