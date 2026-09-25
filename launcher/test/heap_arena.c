/*
 * A first-fit arena allocator that stands in for malloc/calloc/realloc/free
 * in the HOST test build, sized to what this project's device profile says
 * is actually free on the internal heap (device_profiles/esp32s3.sh's
 * DP_FREE_HEAP_BYTES). A laptop-scale heap cannot stand in for
 * the device's: an allocation that fits on the host and not on the board is
 * exactly the failure this exists to catch.
 *
 * FIRST-FIT WITH REAL FRAGMENTATION is the point: a doubly-linked list of
 * address-ordered blocks, failing an allocation exactly when no ONE free
 * block is big enough, even if the sum of several is - the same rule the
 * device's own allocator runs under, which a running-total byte counter
 * would not catch (a 41 KB request can fail on a heap with 50 KB free but
 * no block over 38 KB).
 *
 * Deliberately simple: one header per block (prev/next/size/in_use/magic),
 * first-fit search, split on allocate, coalesce-both-directions on free.
 * This is a test gate, not something a real program should link against.
 *
 * MECHANISM: the host link adds -Wl,--wrap=malloc (and calloc/realloc/free)
 * so every call to those names in the test binary resolves to __wrap_*
 * here instead, with the real libc entry points reachable as __real_*.
 *
 * THE ONE CAVEAT THAT MATTERS: libc-internal allocations do not reliably
 * route through the wrapper. Measured on this toolchain (MinGW-w64
 * x86_64-ucrt gcc 16.1.0): a pointer allocated inside strdup() arrived at
 * __wrap_free() having never been seen by __wrap_malloc() - the CRT's own
 * strdup calls its own already-resolved reference to malloc, not the
 * import our --wrap redirects. So every function below that receives a
 * pointer (free, realloc) MUST tell an arena pointer from a foreign one
 * before touching any block header, and forward the foreign case to
 * __real_free / __real_realloc untouched.
 *
 * Compiled in ONLY when HOST_HEAP_ARENA is defined. launcher/test/timing.c
 * is also compiled into the device firmware (main/CMakeLists.txt) and into an
 * app's own performance-probe harness; neither defines this macro or adds this
 * file to its source list.
 */
#ifdef HOST_HEAP_ARENA

#include "heap_arena.h"

#include "stubs/esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef HOST_HEAP_ARENA_BYTES
#error "heap_arena.c: HOST_HEAP_ARENA_BYTES must be supplied by the build " \
    "(-DHOST_HEAP_ARENA_BYTES=<n>), sourced from a device profile's " \
    "DP_FREE_HEAP_BYTES (launcher/tools/device/device_profiles/esp32s3.sh, read " \
    "via launcher/tools/device/device_profile.sh). A gate running against an " \
    "invented cap is worse than no gate."
#endif

#ifndef HOST_HEAP_ARENA_PSRAM_BYTES
#error "heap_arena.c: HOST_HEAP_ARENA_PSRAM_BYTES must be supplied by the " \
    "build (-DHOST_HEAP_ARENA_PSRAM_BYTES=<n>), sourced from a device " \
    "profile's DP_PSRAM_BYTES - the heap_caps_* pool below models PSRAM " \
    "too now, and it needs a real capacity, not an invented one."
#endif

#ifndef HOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES
#error "heap_arena.c: HOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES must be " \
    "supplied by the build, sourced from a device profile's " \
    "DP_SPIRAM_ALWAYSINTERNAL_BYTES (the board's own " \
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL) - see heap_caps_malloc() below."
#endif

/* Static storage is sized well above HOST_HEAP_ARENA_BYTES so the runtime
 * override below can WIDEN the cap for an experiment without a rebuild -
 * HOST_HEAP_ARENA_BYTES is only the compile-time DEFAULT. 4 MiB costs
 * nothing in a host test binary's .bss. */
#ifndef HOST_HEAP_ARENA_STORAGE_BYTES
#define HOST_HEAP_ARENA_STORAGE_BYTES (4u * 1024u * 1024u)
#endif

/* Same idea, sized above the board's actual 8 MiB of PSRAM for the same
 * widen-without-a-rebuild reason. Still trivial in a host binary's .bss. */
#ifndef HOST_HEAP_ARENA_PSRAM_STORAGE_BYTES
#define HOST_HEAP_ARENA_PSRAM_STORAGE_BYTES (16u * 1024u * 1024u)
#endif

typedef struct arena_block {
    struct arena_block* prev;
    struct arena_block* next;
    size_t size; /* usable payload bytes, excludes this header */
    int in_use;
    unsigned magic; /* set while in_use, checked on free() - catches a
                      * double-free or a foreign pointer that happened to
                      * land inside the arena's byte range */
} arena_block_t;

#define ARENA_MAGIC_LIVE  0xA23EA11Cu
#define ARENA_ALIGN       (sizeof(max_align_t))
#define ARENA_HEADER_SIZE (align_up(sizeof(arena_block_t), ARENA_ALIGN))

static size_t
align_up(size_t n, size_t a) {
    /* a is always a power of two here (sizeof(max_align_t)) */
    return (n + (a - 1)) & ~(a - 1);
}

/* One independently-capped, independently-fragmenting first-fit pool. Two
 * instances below stand in for the board's two allocation pools - internal
 * SRAM and PSRAM - so a request tagged for one can never eat the other's
 * budget, the same separation MALLOC_CAP_* gives the real allocator. */
typedef struct {
    unsigned char* storage;
    size_t storage_bytes;
    size_t default_cap;  /* compile-time default, from the device profile */
    const char* env_var; /* widen-without-rebuild override, see below */
    const char* label;   /* for messages only ("internal" / "psram") */
    arena_block_t* head;
    size_t cap; /* effective cap in bytes, <= storage_bytes */
    size_t cur_bytes;
    size_t cur_blocks;
    size_t peak_bytes;
    int initialized;
} arena_pool_t;

/* _Alignas rather than a plain unsigned char[] - a static array has no
 * alignment guarantee stronger than 1 byte in the standard, and every
 * block header below assumes it can place an arena_block_t at the base. */
static _Alignas(max_align_t) unsigned char s_internal_storage[HOST_HEAP_ARENA_STORAGE_BYTES];
static _Alignas(max_align_t) unsigned char s_psram_storage[HOST_HEAP_ARENA_PSRAM_STORAGE_BYTES];

static arena_pool_t s_internal = {
    .storage = s_internal_storage,
    .storage_bytes = sizeof(s_internal_storage),
    .default_cap = HOST_HEAP_ARENA_BYTES,
    .env_var = "HOST_HEAP_ARENA_BYTES",
    .label = "internal",
};

static arena_pool_t s_psram = {
    .storage = s_psram_storage,
    .storage_bytes = sizeof(s_psram_storage),
    .default_cap = HOST_HEAP_ARENA_PSRAM_BYTES,
    .env_var = "HOST_HEAP_ARENA_PSRAM_BYTES",
    .label = "psram",
};

static int
ptr_in_pool(const arena_pool_t* p, const void* ptr) {
    const unsigned char* b = (const unsigned char*)ptr;
    return b >= p->storage && b < p->storage + p->storage_bytes;
}

/* Reads a pool's compile-time default or, if set, its own environment
 * variable - so a one-off experiment can widen or narrow either cap without
 * a rebuild. Prints the effective cap and where it came from exactly once
 * per pool, since a gate whose cap is silently different from what the last
 * person read in the log is worse than one that never widened at all. */
static size_t
arena_pool_effective_cap(arena_pool_t* p) {
    size_t cap = p->default_cap;
    const char* origin = "compile-time default (device profile, via -D)";

    const char* env = getenv(p->env_var);
    if (env && *env) {
        char* end = NULL;
        unsigned long long v = strtoull(env, &end, 10);
        if (end != env && *end == '\0' && v > 0) {
            cap = (size_t)v;
            origin = "environment override";
        } else {
            fprintf(stderr, "heap_arena: ignoring unparseable %s=%s\n", p->env_var, env);
        }
    }

    if (cap > p->storage_bytes) {
        fprintf(stderr, "heap_arena: %s pool cap %zu exceeds static storage %zu, clamping\n", p->label, cap,
                p->storage_bytes);
        cap = p->storage_bytes;
    }

    printf("heap_arena: %s pool effective cap = %zu bytes (%s)\n", p->label, cap, origin);
    return cap;
}

static void
arena_pool_init_once(arena_pool_t* p) {
    if (p->initialized) {
        return;
    }
    p->cap = arena_pool_effective_cap(p);
    p->head = (arena_block_t*)p->storage;
    p->head->prev = NULL;
    p->head->next = NULL;
    p->head->size = p->cap > ARENA_HEADER_SIZE ? p->cap - ARENA_HEADER_SIZE : 0;
    p->head->in_use = 0;
    p->head->magic = 0;
    p->initialized = 1;
}

/* Splits and hands out the free block b (already known to be big enough),
 * leaving the remainder as a new free block when there is enough of it to
 * be worth a header - a remainder smaller than one more header is folded
 * into this allocation instead of stranding an unusable sliver. */
static void*
arena_pool_take_block(arena_pool_t* p, arena_block_t* b, size_t need) {
    size_t remaining = b->size - need;
    if (remaining >= ARENA_HEADER_SIZE + ARENA_ALIGN) {
        arena_block_t* nb = (arena_block_t*)((unsigned char*)b + ARENA_HEADER_SIZE + need);
        nb->size = remaining - ARENA_HEADER_SIZE;
        nb->in_use = 0;
        nb->magic = 0;
        nb->prev = b;
        nb->next = b->next;
        if (nb->next) {
            nb->next->prev = nb;
        }
        b->next = nb;
        b->size = need;
    }
    b->in_use = 1;
    b->magic = ARENA_MAGIC_LIVE;
    p->cur_bytes += b->size;
    p->cur_blocks += 1;
    if (p->cur_bytes > p->peak_bytes) {
        p->peak_bytes = p->cur_bytes;
    }
    return (unsigned char*)b + ARENA_HEADER_SIZE;
}

/* Walks a pool's own block list for its true free-byte total, true largest
 * free block, and free-block count. Not an approximation: each pool is a
 * real first-fit list, so this is the same quantity a bump-pointer or
 * byte-counter model could only guess at - see
 * heap_caps_get_largest_free_block() below for what that honesty does and
 * does not buy. */
static void
arena_pool_scan(arena_pool_t* p, size_t* out_total_free, size_t* out_largest_free, size_t* out_free_blocks) {
    arena_pool_init_once(p);
    size_t total = 0, largest = 0, blocks = 0;
    for (arena_block_t* b = p->head; b; b = b->next) {
        if (!b->in_use) {
            total += b->size;
            blocks += 1;
            if (b->size > largest) {
                largest = b->size;
            }
        }
    }
    if (out_total_free) {
        *out_total_free = total;
    }
    if (out_largest_free) {
        *out_largest_free = largest;
    }
    if (out_free_blocks) {
        *out_free_blocks = blocks;
    }
}

/* First-fit search plus, on failure, the same story a device OOM would
 * give ("41.2 KiB needed, 38 KiB largest free block") -
 * printed here rather than left for the caller to reconstruct from a bare
 * NULL. */
static void*
arena_pool_alloc(arena_pool_t* p, size_t n) {
    arena_pool_init_once(p);
    if (n == 0) {
        n = 1; /* malloc(0): return a distinct, freeable pointer, not NULL */
    }
    size_t need = align_up(n, ARENA_ALIGN);

    for (arena_block_t* b = p->head; b; b = b->next) {
        if (!b->in_use && b->size >= need) {
            return arena_pool_take_block(p, b, need);
        }
    }

    size_t total_free, largest_free, free_blocks;
    arena_pool_scan(p, &total_free, &largest_free, &free_blocks);
    fprintf(stderr,
            "heap_arena: %s pool alloc of %zu bytes FAILED - %zu needed, %zu "
            "bytes largest free block, %zu bytes free across %zu block(s), "
            "cap %zu\n",
            p->label, n, need, largest_free, total_free, free_blocks, p->cap);
    return NULL;
}

/* Merges b with a free neighbour on either side. Address-ordered list, so
 * "neighbour" here means adjacent in the list, which is also adjacent in
 * memory by construction (every split creates its remainder immediately
 * after the block it came from). */
static void
arena_pool_release(arena_pool_t* p, arena_block_t* b) {
    b->in_use = 0;
    b->magic = 0;
    p->cur_bytes -= b->size;
    p->cur_blocks -= 1;

    if (b->next && !b->next->in_use) {
        arena_block_t* n = b->next;
        b->size += ARENA_HEADER_SIZE + n->size;
        b->next = n->next;
        if (b->next) {
            b->next->prev = b;
        }
    }
    if (b->prev && !b->prev->in_use) {
        arena_block_t* pr = b->prev;
        pr->size += ARENA_HEADER_SIZE + b->size;
        pr->next = b->next;
        if (pr->next) {
            pr->next->prev = pr;
        }
    }
}

/* Caller must already know ptr is inside this pool - see ptr_in_pool()
 * calls at every call site below. Aborts on a bad header instead of
 * silently corrupting the list: a double-free or an in-pool-but-not-a-
 * live-block pointer is a real bug, and a test gate that swallows it
 * defeats the point of running under a byte-for-byte-accurate allocator. */
static void
arena_pool_free(arena_pool_t* p, void* ptr) {
    arena_block_t* b = (arena_block_t*)((unsigned char*)ptr - ARENA_HEADER_SIZE);
    if (!ptr_in_pool(p, b) || b->magic != ARENA_MAGIC_LIVE) {
        fprintf(stderr,
                "heap_arena: free(%p) in the %s pool does not look like a "
                "live arena block - double free, corruption, or a pointer "
                "this pool never issued\n",
                ptr, p->label);
        abort();
    }
    arena_pool_release(p, b);
}

void
heap_arena_snapshot(size_t* out_blocks, size_t* out_bytes) {
    arena_pool_init_once(&s_internal);
    if (out_blocks) {
        *out_blocks = s_internal.cur_blocks;
    }
    if (out_bytes) {
        *out_bytes = s_internal.cur_bytes;
    }
}

size_t
heap_arena_peak_bytes(void) {
    return s_internal.peak_bytes;
}

void
heap_arena_reset_peak(void) {
    arena_pool_init_once(&s_internal);
    /* Floored at what's already outstanding, not zeroed - a test that
     * starts after an earlier leak should show that leak weighing on its
     * own peak, the same way it would starve a real boot. */
    s_internal.peak_bytes = s_internal.cur_bytes;
}

/* malloc/calloc/realloc/free interposition */
/*
 * Everything below this point charges the INTERNAL pool only, exactly as
 * before PSRAM was modeled - plain malloc()/calloc()/free() never routes to
 * PSRAM on their own; only an explicit heap_caps_* call (below) can.
 */

extern void* __real_malloc(size_t size);
extern void __real_free(void* ptr);
extern void* __real_realloc(void* ptr, size_t size);

void*
__wrap_malloc(size_t size) {
    return arena_pool_alloc(&s_internal, size);
}

void
__wrap_free(void* ptr) {
    if (!ptr) {
        return;
    }
    if (!ptr_in_pool(&s_internal, ptr)) {
        /* Almost certainly a libc-internal allocation (strdup() and
         * friends) that never went through __wrap_malloc - see this
         * file's top comment. Forward it rather than misread foreign
         * bytes as one of our headers. */
        __real_free(ptr);
        return;
    }
    arena_pool_free(&s_internal, ptr);
}

void*
__wrap_calloc(size_t nmemb, size_t size) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) {
        return NULL; /* overflow - same contract calloc itself makes */
    }
    size_t total = nmemb * size;
    void* p = arena_pool_alloc(&s_internal, total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void*
__wrap_realloc(void* ptr, size_t size) {
    if (!ptr) {
        return arena_pool_alloc(&s_internal, size);
    }
    if (!ptr_in_pool(&s_internal, ptr)) {
        /* Foreign pointer - see __wrap_free above for why this can happen
         * at all. Hand it to the real realloc untouched. */
        return __real_realloc(ptr, size);
    }
    if (size == 0) {
        arena_pool_free(&s_internal, ptr);
        return NULL;
    }

    arena_block_t* b = (arena_block_t*)((unsigned char*)ptr - ARENA_HEADER_SIZE);
    size_t need = align_up(size, ARENA_ALIGN);
    if (need <= b->size) {
        /* Shrinking (or same size) in place. No split on shrink - kept
         * simple on purpose, this is a test gate, not a production
         * allocator, and the extra fragmentation from not splitting here
         * only ever makes a fixture's job HARDER, never easier, so it
         * cannot hide a device failure. */
        return ptr;
    }

    void* grown = arena_pool_alloc(&s_internal, size);
    if (!grown) {
        return NULL; /* realloc's own contract: leave the original intact */
    }
    memcpy(grown, ptr, b->size);
    arena_pool_free(&s_internal, ptr);
    return grown;
}

/* heap_caps_* models the board's two pools (board.h's BOARD_FRAMEBUFFER_CAPS,
 * the sdkconfig's CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL). An explicit
 * MALLOC_CAP_SPIRAM/_INTERNAL/_DMA request never spills into the other pool
 * on failure - that spill is the device-only failure this exists to catch.
 * A bare MALLOC_CAP_8BIT/_DEFAULT is the one path with a fallback, since
 * ALWAYSINTERNAL is itself a fallback rule on the device. */

static int
caps_matches_internal(uint32_t caps) {
    return (caps & MALLOC_CAP_SPIRAM) == 0;
}

/* MALLOC_CAP_DMA excludes psram along with MALLOC_CAP_INTERNAL: PSRAM
 * carries neither cap on this board (gfx.c bounces every PSRAM strip
 * through an internal DMA buffer rather than DMA'ing PSRAM directly). */
static int
caps_matches_psram(uint32_t caps) {
    return (caps & (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)) == 0;
}

static arena_pool_t*
heap_caps_pick_pool(uint32_t caps, size_t size, int* try_other) {
    *try_other = 0;
    if (!caps_matches_psram(caps)) {
        return &s_internal;
    }
    if (!caps_matches_internal(caps)) {
        return &s_psram;
    }
    *try_other = 1;
    return size <= (size_t)HOST_HEAP_ARENA_ALWAYSINTERNAL_BYTES ? &s_internal : &s_psram;
}

void*
heap_caps_malloc(size_t size, uint32_t caps) {
    int try_other = 0;
    arena_pool_t* first = heap_caps_pick_pool(caps, size, &try_other);
    void* p = arena_pool_alloc(first, size);
    if (p || !try_other) {
        return p;
    }
    return arena_pool_alloc(first == &s_internal ? &s_psram : &s_internal, size);
}

void*
heap_caps_calloc(size_t nmemb, size_t size, uint32_t caps) {
    if (nmemb != 0 && size > (size_t)-1 / nmemb) {
        return NULL;
    }
    size_t total = nmemb * size;
    void* p = heap_caps_malloc(total, caps);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void
heap_caps_free(void* ptr) {
    if (!ptr) {
        return;
    }
    if (ptr_in_pool(&s_internal, ptr)) {
        arena_pool_free(&s_internal, ptr);
        return;
    }
    if (ptr_in_pool(&s_psram, ptr)) {
        arena_pool_free(&s_psram, ptr);
        return;
    }
    fprintf(stderr, "heap_caps_free(%p): not inside either modeled pool\n", ptr);
    abort();
}

size_t
heap_caps_get_free_size(uint32_t caps) {
    size_t total = 0, t, l, b;
    if (caps_matches_internal(caps)) {
        arena_pool_scan(&s_internal, &t, &l, &b);
        total += t;
    }
    if (caps_matches_psram(caps)) {
        arena_pool_scan(&s_psram, &t, &l, &b);
        total += t;
    }
    return total;
}

/* Honest about fragmentation, and about the limits of that honesty: this is
 * the TRUE largest free block in OUR modeled pool (a real first-fit list,
 * scanned exactly, not estimated - see arena_pool_scan() above), never a
 * prediction of the device's own fragmentation. That depends on the
 * device's actual allocation order and history, which a host test does not
 * replay, so this number can legitimately differ from a real capture even
 * when every byte total matches. */
size_t
heap_caps_get_largest_free_block(uint32_t caps) {
    size_t best = 0, t, l, b;
    if (caps_matches_internal(caps)) {
        arena_pool_scan(&s_internal, &t, &l, &b);
        if (l > best) {
            best = l;
        }
    }
    if (caps_matches_psram(caps)) {
        arena_pool_scan(&s_psram, &t, &l, &b);
        if (l > best) {
            best = l;
        }
    }
    return best;
}

#endif /* HOST_HEAP_ARENA */
