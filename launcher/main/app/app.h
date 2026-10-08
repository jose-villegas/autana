/*
 * app: the contract between the shell and the things it launches.
 *
 * "Apps" here are not processes. There is one binary, one address space and
 * one core; an app is a set of callbacks the shell drives. That keeps
 * switching instant and costs no flash partitions, at the price of apps not
 * being isolated from each other: a misbehaving app can corrupt the shell.
 *
 * An app never owns the screen or the frame loop. It draws into the shared
 * framebuffer when asked and returns; the shell decides when to present, and
 * paints its own chrome on top afterwards.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gfx/present/gfx_band_run.h"
#include "input/input.h"

/* An app's own console command: docs/tools/Autana-CLI.md's "Adding a
 * command from an app". The shell matches `prefix` as a whole word
 * (console_word_match()) and passes `handle` only what follows it. */
typedef struct {
    const char* prefix;
    bool (*handle)(const char* args);
} app_console_t;

#if CONFIG_LAUNCHER_DEVELOPMENT
/* The only way to fill one: APP_CONSOLE() at top level, then
 * APP_CONSOLE_PTR(handler) as the app_t's `.console`. Two macros, since the
 * assert is a declaration. Clashes and long prefixes are checked at boot
 * (console_find_clash(), shell/shell.c). */
#define APP_CONSOLE(prefix, handler)                                                                                   \
    _Static_assert(sizeof(prefix) > 1, "APP_CONSOLE needs a non-empty prefix");                                        \
    static const app_console_t handler##_console = {(prefix), (handler)}
#define APP_CONSOLE_PTR(handler) (&handler##_console)
#else
/* static inline: unused and uncalled, so an optimizing linker drops it and
 * `handler` behind it, without the -Wunused-function a plain static would
 * draw for a handler an app still defines unconditionally. */
#define APP_CONSOLE(prefix, handler)                                                                                   \
    static inline void handler##_console_unused(void) { (void)(handler); }
#define APP_CONSOLE_PTR(handler) NULL
#endif

typedef struct app {
    const char* name;
    const char* summary; /* one line, shown in the launcher list */

    /* Called once as the app starts. Use it to reset state; there is no
     * guarantee the app has not run before. */
    void (*enter)(void);

    /* Called once per frame. Draw into the shared framebuffer via gfx.
     * `dt_ms` is the time since the previous frame, for animation that should
     * not depend on framerate. */
    void (*frame)(uint32_t dt_ms, const input_t* input);

    /* Optional. An app that entered GFX_LAYOUT_BANDS supplies the rows for
     * each dirty band; gfx replays UI commands and sends the finished band. */
    gfx_band_draw_fn draw_band;

    /* Optional, NULL unless an app sets it. When present, the shell overlaps
     * it with sending the PREVIOUS frame() call's output on core 1
     * (gfx_present_begin()/gfx_present_wait(), gfx_present.h). update() may change
     * app state but MUST NOT call any gfx_* function or touch the
     * framebuffer: that buffer may still be mid-send. A development build
     * asserts this (see gfx_present_guard.h). Left NULL: frame(), then
     * gfx_present(). */
    void (*update)(uint32_t dt_ms, const input_t* input);

    /* Called once as the app stops. Release anything enter() acquired. */
    void (*exit)(void);

    /* Opt-in, NULL unless an app keeps a draw cache of its own beyond the
     * framebuffer: row-run spans, a partial-clear bbox, and so on. The
     * shell calls this once, before the next frame() after
     * gfx_request_full_redraw() (gfx_present.h) was called by the shell or by the
     * app itself, so that cache can be reset the same way the framebuffer
     * already was. An app with no such cache needs no implementation. */
    void (*invalidate)(void);

    /* Opt-in: the shell tracks the swipe-home gesture and draws its hint
     * strip only for an app that sets this. An app whose own input is a
     * drag near an edge leaves it off and offers its own way home. */
    bool home_gesture;

    /* Opt-in, like home_gesture above: NULL unless an app sets it. If
     * set, called only from console_screenshot_dump()
     * (CONFIG_LAUNCHER_DEVELOPMENT builds only) to let the running app
     * attach its own state to a screenshot capture: a JSON OBJECT fragment
     * (starting with `{`,
     * ending with `}`, no trailing comma) written into `out` (at most
     * `len` bytes, NUL-terminated). Spliced into the capture's
     * device-state JSON as a new "app" key. Diagnostic only: nothing
     * about the app's own behaviour depends on this. */
    void (*diagnostic_json)(char* out, size_t len);

    /* Opt-in, NULL unless an app declares one with APP_CONSOLE_PTR() above.
     * See docs/tools/Autana-CLI.md's "Adding a command from an app". */
    const app_console_t* console;

    /* The registry's link, set by app_register(); an app never sets it. */
    struct app* next;
} app_t;

/* Leaves the running app for the launcher before its next frame, as the
 * home gesture does, for an app with an on-screen way out. */
void shell_request_exit(void);

/*
 * APP_REGISTER() places a constructor in .init_array, which ESP-IDF runs
 * before app_main(). Link order decides .init_array order; app_register()
 * sorts by name at insertion so it never shows.
 */

/* Called by APP_REGISTER before main(); defined in app_registry.c. */
void app_register(app_t* app);

#define APP_REGISTER(symbol)                                                                                           \
    __attribute__((constructor)) static void symbol##_register(void) { app_register(&symbol); }

/* The head of the registered apps, sorted by name and linked through
 * app_t.next, NULL-terminated; NULL itself when nothing is registered.
 * Valid from the first line of app_main(). */
const app_t* app_list(void);

#ifndef ESP_PLATFORM
/* Host-only, absent from every device build: a test process runs many
 * scenarios in one run and each wants a clean list, unlike a real boot's
 * one-shot registration. */
void app_registry_reset_for_test(void);
#endif
