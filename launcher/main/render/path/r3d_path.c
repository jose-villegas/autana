#include "render/path/r3d_path.h"

/* The board has no paths, and so no list. */
#if R3D_PATHS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static r3d_path_t* first;

void
r3d_path_register(r3d_path_t* path) {
    r3d_path_t** at = &first;
    while (*at != NULL && strcmp((*at)->name, path->name) < 0) {
        at = &(*at)->next;
    }
    if (*at != NULL && strcmp((*at)->name, path->name) == 0 && strcmp((*at)->file, path->file) != 0) {
        (void)fprintf(stderr, "r3d_path: %s is declared in %s and in %s\n", path->name, (*at)->file, path->file);
        abort();
    }
    path->next = *at;
    *at = path;
}

const r3d_path_t*
r3d_path_first(void) {
    return first;
}

void
r3d_path_forget(void) {
    for (r3d_path_t* path = first; path != NULL; path = path->next) {
        path->hits = 0;
    }
}

#else

typedef int r3d_path_has_nothing_on_the_board_t;

#endif
