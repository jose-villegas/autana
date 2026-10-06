#define _DEFAULT_SOURCE /* MAP_ANONYMOUS */

#include "test_fence.h"

#include <stdbool.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

static size_t
page_size(void) {
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return info.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

static void*
map_pages(size_t size) {
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void* region = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return region == MAP_FAILED ? NULL : region;
#endif
}

static bool
seal_page(void* page, size_t size) {
#ifdef _WIN32
    DWORD old;
    return VirtualProtect(page, size, PAGE_NOACCESS, &old) != 0;
#else
    return mprotect(page, size, PROT_NONE) == 0;
#endif
}

static void
unmap_pages(void* region, size_t size) {
#ifdef _WIN32
    (void)size;
    VirtualFree(region, 0, MEM_RELEASE);
#else
    munmap(region, size);
#endif
}

test_fence_t
test_fence_alloc(size_t size) {
    const size_t page = page_size();
    const size_t readable = (size + page - 1U) / page * page;
    uint8_t* region = map_pages(readable + page);
    if (region == NULL) {
        return (test_fence_t){0};
    }
    if (!seal_page(region + readable, page)) {
        unmap_pages(region, readable + page);
        return (test_fence_t){0};
    }
    return (test_fence_t){.bytes = region + readable - size, .region = region, .region_size = readable + page};
}

void
test_fence_free(test_fence_t* fence) {
    if (fence->region != NULL) {
        unmap_pages(fence->region, fence->region_size);
    }
    *fence = (test_fence_t){0};
}
