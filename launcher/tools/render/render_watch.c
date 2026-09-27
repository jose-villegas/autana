#if !defined(_WIN32)
#define _POSIX_C_SOURCE 200809L
#endif

#include "render_watch.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/frame_watch.h"

#if defined(_WIN32)
#include <io.h>
#define watch_dup    _dup
#define watch_dup2   _dup2
#define watch_fileno _fileno
#define watch_close  _close
#else
#include <unistd.h>
#define watch_dup    dup
#define watch_dup2   dup2
#define watch_fileno fileno
#define watch_close  close
#endif

/* Every stdout write in a frame is one site: a host has no caller to tell
 * two prints apart, and the text itself is shown instead. */
#define CONSOLE_SITE  ((uintptr_t)0)

#define SNIPPET_BYTES 120

void* __real_malloc(size_t size);
void* __real_calloc(size_t count, size_t size);
void* __real_realloc(void* ptr, size_t size);
void __real_free(void* ptr);

static frame_watch_t watch;
static bool inside;
static const char* binary_name;
static const char* capture_path;
static int saved_stdout = -1;
static long frame_console_start;
static long last_console_start;
static long last_console_end;

static void
note(frame_watch_kind_t kind, void* caller) {
    if (inside) {
        frame_watch_note(&watch, kind, (uintptr_t)caller);
    }
}

void*
__wrap_malloc(size_t size) {
    note(FRAME_WATCH_ALLOC, __builtin_return_address(0));
    return __real_malloc(size);
}

void*
__wrap_calloc(size_t count, size_t size) {
    note(FRAME_WATCH_ALLOC, __builtin_return_address(0));
    return __real_calloc(count, size);
}

void*
__wrap_realloc(void* ptr, size_t size) {
    note(FRAME_WATCH_ALLOC, __builtin_return_address(0));
    return __real_realloc(ptr, size);
}

void
__wrap_free(void* ptr) {
    if (ptr != NULL) {
        note(FRAME_WATCH_FREE, __builtin_return_address(0));
    }
    __real_free(ptr);
}

#if defined(_WIN32)
/* The base the linker chose, read from the file: the loader rewrites the
 * header it maps to the base it actually used. */
static uint64_t
preferred_image_base(void) {
    uint64_t base = 0;
    FILE* image = fopen(binary_name, "rb");
    if (image == NULL) {
        return 0;
    }
    uint32_t nt_headers = 0;
    if (fseek(image, 0x3c, SEEK_SET) == 0 && fread(&nt_headers, sizeof nt_headers, 1, image) == 1
        && fseek(image, (long)nt_headers + 24 + 24, SEEK_SET) == 0 && fread(&base, sizeof base, 1, image) == 1) {
        fclose(image);
        return base;
    }
    fclose(image);
    return 0;
}
#endif

/* The address addr2line wants: where the site sits in the file, whatever
 * base the loader chose this run. */
static uintptr_t
file_address(uintptr_t site) {
#if defined(_WIN32)
    extern const unsigned char __ImageBase[];
    return site - (uintptr_t)__ImageBase + (uintptr_t)preferred_image_base();
#elif defined(__ELF__) && defined(__PIE__)
    extern const char __executable_start[];
    return site - (uintptr_t)__executable_start;
#else
    return site;
#endif
}

static long
console_bytes(void) {
    if (capture_path == NULL) {
        return 0;
    }
    fflush(stdout);
    const long at = ftell(stdout);
    return at < 0 ? 0 : at;
}

static void
print_console_snippet(void) {
    FILE* captured = fopen(capture_path, "rb");
    if (captured == NULL) {
        return;
    }
    char text[SNIPPET_BYTES + 1];
    size_t want = (size_t)(last_console_end - last_console_start);
    want = want < SNIPPET_BYTES ? want : SNIPPET_BYTES;
    size_t got = 0;
    if (fseek(captured, last_console_start, SEEK_SET) == 0) {
        got = fread(text, 1, want, captured);
    }
    fclose(captured);
    text[got] = '\0';
    fprintf(stderr, "  the last frame printed: %.*s\n", (int)strcspn(text, "\n"), text);
}

static void
warn(const frame_watch_site_t* s) {
    const char* kind = frame_watch_kind_name((frame_watch_kind_t)s->kind);
    const int seen = frame_watch_frames_seen(s);
    if (s->kind == FRAME_WATCH_CONSOLE) {
        fprintf(stderr, "FRAME_WATCH %s in %d of %d frames: stdout\n", kind, seen, FRAME_WATCH_WINDOW);
        print_console_snippet();
        return;
    }
    fprintf(stderr, "FRAME_WATCH %s in %d of %d frames at %p: addr2line -f -i -e %s 0x%llx\n", kind, seen,
            FRAME_WATCH_WINDOW, (void*)s->site, binary_name, (unsigned long long)file_address(s->site));
}

bool
render_watch_start(const char* binary, const char* console_path) {
    frame_watch_reset(&watch);
    binary_name = binary;
    if (console_path == NULL) {
        return true;
    }
    fflush(stdout);
    FILE* capture = fopen(console_path, "w+b");
    if (capture == NULL) {
        return false;
    }
    saved_stdout = watch_dup(watch_fileno(stdout));
    const bool redirected = saved_stdout >= 0 && watch_dup2(watch_fileno(capture), watch_fileno(stdout)) != -1;
    fclose(capture);
    if (!redirected) {
        if (saved_stdout >= 0) {
            watch_close(saved_stdout);
            saved_stdout = -1;
        }
        remove(console_path);
        return false;
    }
    capture_path = console_path;
    return true;
}

void
render_watch_frame_begin(void) {
    frame_console_start = console_bytes();
    inside = true;
}

void
render_watch_frame_end(void) {
    inside = false;
    const long end = console_bytes();
    if (end > frame_console_start) {
        frame_watch_note(&watch, FRAME_WATCH_CONSOLE, CONSOLE_SITE);
        last_console_start = frame_console_start;
        last_console_end = end;
    }
    frame_watch_close_frame(&watch);
    for (int i = 0; i < FRAME_WATCH_SITES; i++) {
        if (frame_watch_take_due(&watch.sites[i], 0)) {
            warn(&watch.sites[i]);
        }
    }
}

/* Whatever the scene printed reaches the real stdout after all, in order. */
static void
replay_console(void) {
    fflush(stdout);
    FILE* captured = fopen(capture_path, "rb");
    watch_dup2(saved_stdout, watch_fileno(stdout));
    watch_close(saved_stdout);
    saved_stdout = -1;
    if (captured != NULL) {
        char buffer[4096];
        size_t got;
        while ((got = fread(buffer, 1, sizeof buffer, captured)) > 0) {
            fwrite(buffer, 1, got, stdout);
        }
        fclose(captured);
        fflush(stdout);
    }
    remove(capture_path);
    capture_path = NULL;
}

frame_watch_verdict_t
render_watch_finish(void) {
    if (capture_path != NULL) {
        replay_console();
    }
    const frame_watch_verdict_t verdict = frame_watch_verdict(&watch);
    fprintf(stderr, "FRAME_WATCH judged %lu frames after a %d-frame warm-up: %d sites repeating, %lu events dropped\n",
            (unsigned long)verdict.frames, FRAME_WATCH_WARMUP, verdict.repeating, (unsigned long)verdict.dropped);
    return verdict;
}
