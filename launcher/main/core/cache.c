#include "core/cache.h"

#if defined(ESP_PLATFORM) && !CONFIG_LAUNCHER_QEMU
#include <assert.h>

#include "esp32s3/rom/cache.h"
#include "soc/extmem_reg.h"
#include "soc/soc.h"

#define AUTOLOAD_LINES 1

/* extmem_reg.h specifies a virtual start and byte length; SIZE counts blocks,
 * RQST 0 means miss, ORDER 0 ascending. rom/cache.h's CACHE_AUTOLOAD_STEP
 * encodes blocks minus one. The ROM region API warns against SDK use;
 * direct registers preserve the caller's complete configuration. */
static void
cache_autoload_stop(uint32_t ctrl) {
    REG_WRITE(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG, ctrl & ~EXTMEM_DCACHE_AUTOLOAD_ENA);
    /* extmem_reg.h pairs ENA with DONE: drain the operation before changing
     * its region, including when restoring an enclosing scope. */
    while (!(REG_READ(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG) & EXTMEM_DCACHE_AUTOLOAD_DONE)) {}
}

cache_autoload_state_t
cache_autoload_push(const void* addr, size_t size) {
    assert((uintptr_t)addr <= UINT32_MAX);
    assert(size <= EXTMEM_DCACHE_AUTOLOAD_SCT0_SIZE_V);
    const cache_autoload_state_t previous = {
        .ctrl = REG_READ(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG),
        .addr = REG_READ(EXTMEM_DCACHE_AUTOLOAD_SCT0_ADDR_REG),
        .size = REG_READ(EXTMEM_DCACHE_AUTOLOAD_SCT0_SIZE_REG),
    };
    cache_autoload_stop(previous.ctrl);
    REG_WRITE(EXTMEM_DCACHE_AUTOLOAD_SCT0_ADDR_REG, (uint32_t)(uintptr_t)addr);
    REG_WRITE(EXTMEM_DCACHE_AUTOLOAD_SCT0_SIZE_REG, (uint32_t)size);
    const uint32_t fields = EXTMEM_DCACHE_AUTOLOAD_SIZE_M | EXTMEM_DCACHE_AUTOLOAD_RQST_M
                            | EXTMEM_DCACHE_AUTOLOAD_ORDER_M | EXTMEM_DCACHE_AUTOLOAD_SCT1_ENA
                            | EXTMEM_DCACHE_AUTOLOAD_SCT0_ENA | EXTMEM_DCACHE_AUTOLOAD_ENA;
    const uint32_t config = (CACHE_AUTOLOAD_STEP(AUTOLOAD_LINES) << EXTMEM_DCACHE_AUTOLOAD_SIZE_S)
                            | (CACHE_AUTOLOAD_MISS_TRIGGER << EXTMEM_DCACHE_AUTOLOAD_RQST_S)
                            | (CACHE_AUTOLOAD_POSITIVE << EXTMEM_DCACHE_AUTOLOAD_ORDER_S);
    const uint32_t enable = size ? EXTMEM_DCACHE_AUTOLOAD_SCT0_ENA | EXTMEM_DCACHE_AUTOLOAD_ENA : 0;
    REG_WRITE(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG, (previous.ctrl & ~fields) | config | enable);
    return previous;
}

void
cache_autoload_pop(cache_autoload_state_t previous) {
    cache_autoload_stop(REG_READ(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG));
    REG_WRITE(EXTMEM_DCACHE_AUTOLOAD_SCT0_ADDR_REG, previous.addr);
    REG_WRITE(EXTMEM_DCACHE_AUTOLOAD_SCT0_SIZE_REG, previous.size);
    REG_WRITE(EXTMEM_DCACHE_AUTOLOAD_CTRL_REG, previous.ctrl);
}
#endif
