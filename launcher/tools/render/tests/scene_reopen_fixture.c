/* A run closed and the same scene id opened again from another folder in one process. */
#include <stdlib.h>

#include "../scene_viewer.c"
#include "asset/asset_store.h"

#define REOPEN_A_DIR_ENV   "REOPEN_PACKS_A"
#define REOPEN_B_DIR_ENV   "REOPEN_PACKS_B"
#define REOPEN_B_OUT_ENV   "REOPEN_OUT_B"
#define REOPEN_BAD_DIR_ENV "REOPEN_PACKS_BAD"

static void
point_packs_at(const char* dir) {
#ifdef _WIN32
    (void)_putenv_s(ASSET_STORE_DIR_ENV, dir);
#else
    (void)setenv(ASSET_STORE_DIR_ENV, dir, 1);
#endif
}

/* One run of `argv` with the frames it draws written to its -o; true when it opened and closed. */
static bool
run_once(int argc, char** argv, bool* opened) {
    int exit_code;
    *opened = render_host_open(&scene_provider, argc, argv, &exit_code);
    if (!*opened) {
        return false;
    }
    const input_t input = {0};
    for (int i = 0; i < render_host_frame()->count; i++) {
        render_host_step(render_host_frame()->dt_ms, &input);
    }
    return render_host_close();
}

static bool
nothing_left(const char* when) {
    if (asset_store_mounted() == 0) {
        return true;
    }
    fprintf(stderr, "reopen: %d pack(s) still mounted %s\n", asset_store_mounted(), when);
    return false;
}

int
main(int argc, char** argv) {
    const char* b_dir = getenv(REOPEN_B_DIR_ENV);
    const char* b_out = getenv(REOPEN_B_OUT_ENV);
    const char* bad_dir = getenv(REOPEN_BAD_DIR_ENV);
    const char* a_dir = getenv(REOPEN_A_DIR_ENV);
    if (b_dir == NULL || b_out == NULL || bad_dir == NULL || a_dir == NULL) {
        fprintf(stderr, "reopen: needs " REOPEN_A_DIR_ENV ", " REOPEN_B_DIR_ENV ", " REOPEN_B_OUT_ENV
                        " and " REOPEN_BAD_DIR_ENV "\n");
        return 2;
    }
    bool opened;
    if (!run_once(argc, argv, &opened) || !nothing_left("after the first run")) {
        return 1;
    }
    /* The same scene id from a folder whose pack differs: the frames must be that pack's. */
    char** second = malloc(sizeof *second * (size_t)argc);
    memcpy(second, argv, sizeof *second * (size_t)argc);
    int second_argc = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            i++; /* the options of the run before must not outlast it */
            continue;
        }
        second[second_argc++] = argv[i];
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            second[second_argc++] = (char*)b_out;
            i++;
        }
    }
    point_packs_at(b_dir);
    const bool second_ok = run_once(second_argc, second, &opened);
    free(second);
    if (!second_ok || !nothing_left("after the second run")) {
        return 1;
    }
    /* A pack the engine refuses stays refused rather than the first folder's answering. */
    point_packs_at(bad_dir);
    if (run_once(argc, argv, &opened)) {
        fprintf(stderr, "reopen: a pack of another version opened\n");
        return 1;
    }
    if (!nothing_left("after the refused open")) {
        return 1;
    }
    /* And a good pack opens after the refusal, drawing its own frames. */
    point_packs_at(a_dir);
    return run_once(argc, argv, &opened) && nothing_left("after the run after the refusal") ? 0 : 1;
}
