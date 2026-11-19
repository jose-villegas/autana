/*
 * r3d_path: a hit counter on each of the renderer's fill paths, on a host
 * only. A path is declared once at file scope beside the code that takes it,
 * R3D_PATH(id, "name"), and counted where that code runs, R3D_PATH_HIT(id).
 * Every declared path joins one list when the program starts, reached or
 * not, so a reader can name the paths nothing reached. The counts observe
 * and steer nothing. A path counts in its own unit (a triangle, a span or a
 * row stretch, as its site says), so counts compare within a path, not across.
 *
 * On the board (ESP_PLATFORM) both macros are nothing, so no device build
 * holds a counter or the list; check_release_symbols.sh fails a release
 * image that does.
 */
#pragma once

#include <stdint.h>

#ifndef ESP_PLATFORM

#define R3D_PATHS 1

typedef struct r3d_path {
    const char* name;
    const char* file; /* the source declaring it, without its folders */
    uint64_t hits;
    struct r3d_path* next;
} r3d_path_t;

/* Joins `path` to the list, in name order. A test that includes a source
 * file holds a second copy of its paths: a name already there from a file
 * of the same name joins beside it, and one from another file aborts. */
void r3d_path_register(r3d_path_t* path);

/* Every declared path, in name order; a path's copies are neighbours, so a
 * reader sums each name's run. */
const r3d_path_t* r3d_path_first(void);

/* Zeroes every path's hits. */
void r3d_path_forget(void);

#define R3D_PATH(id_, name_)                                                                                           \
    static r3d_path_t r3d_path_##id_ = {name_, __FILE_NAME__, 0, NULL};                                                \
    __attribute__((constructor)) static void r3d_path_##id_##_register(void) { r3d_path_register(&r3d_path_##id_); }   \
    _Static_assert(1, name_)

/* Relaxed: a draw split across worker threads counts every hit. */
#define R3D_PATH_HIT(id_) ((void)__atomic_fetch_add(&r3d_path_##id_.hits, 1, __ATOMIC_RELAXED))

#else

#define R3D_PATHS            0
#define R3D_PATH(id_, name_) _Static_assert(1, name_)
#define R3D_PATH_HIT(id_)    ((void)0)

#endif
