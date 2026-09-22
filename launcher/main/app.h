/*
 * app - the contract between the shell and the things it launches.
 *
 * "Apps" here are not processes. There is one binary, one address space and
 * one core; an app is a set of callbacks the shell drives. That keeps
 * switching instant and costs no flash partitions, at the price of apps not
 * being isolated from each other - a misbehaving app can corrupt the shell.
 *
 * An app never owns the screen or the frame loop. It draws into the shared
 * framebuffer when asked and returns; the shell decides when to present, and
 * paints its own chrome on top afterwards.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "input/input.h"

/* An app's own console command - docs/tools/Autana-CLI.md's "Adding a
 * command from an app". The shell matches `prefix` as a whole word
 * (console_word_match()) and passes `handle` only what follows it. */
typedef struct {
    const char* prefix;
    bool (*handle)(const char* args);
} app_console_t;

#if CONFIG_LAUNCHER_DEVELOPMENT
/* APP_CONSOLE() (a top-level declaration, before the app's own app_t) plus
 * APP_CONSOLE_PTR(handler) (that app_t's `.console = `) are the only
 * sanctioned way to fill one: two macros because the assert below is a
 * declaration, which cannot sit inside app_t's own constant initializer.
 * A clash and an over-long prefix are both checked at boot instead
 * (console_find_clash(), main.c) - this app.h stays clear of console/, so
 * an app pulls in only what it names. */
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

typedef struct {
    const char* name;
    const char* summary; /* one line, shown in the launcher list */

    /* Called once as the app starts. Use it to reset state; there is no
     * guarantee the app has not run before. */
    void (*enter)(void);

    /* Called once per frame. Draw into the shared framebuffer via gfx.
     * `dt_ms` is the time since the previous frame, for animation that should
     * not depend on framerate. */
    void (*frame)(uint32_t dt_ms, const input_t* input);

    /* Optional, NULL unless an app sets it. When present, the shell overlaps
     * it with sending the PREVIOUS frame() call's output on core 1
     * (gfx_present_begin()/gfx_present_wait(), gfx.h). update() may change
     * app state but MUST NOT call any gfx_* function or touch the
     * framebuffer - that buffer may still be mid-send. A development build
     * asserts this (see gfx_present_guard.h). Left NULL: frame(), then
     * gfx_present(). */
    void (*update)(uint32_t dt_ms, const input_t* input);

    /* Called once as the app stops. Release anything enter() acquired. */
    void (*exit)(void);

    /* Opt-in, NULL unless an app keeps a draw cache of its own beyond the
     * framebuffer - row-run spans, a partial-clear bbox, and so on. The
     * shell calls this once, before the next frame() after
     * gfx_request_full_redraw() (gfx.h) was called by the shell or by the
     * app itself, so that cache can be reset the same way the framebuffer
     * already was. An app with no such cache needs no implementation. */
    void (*invalidate)(void);

    /* Opt-in, not opt-out: false unless an app sets it. main.c only
     * tracks the edge-swipe-home gesture and draws its hint strip while
     * an app with this true is running - an app that leaves it unset
     * gets neither, and is responsible for its own way back to the
     * launcher. It exists for an app whose own input is a touch drag
     * near a screen edge, which the swipe-home gesture cannot be told
     * apart from: such an app turns the generic one off and offers a
     * deliberate control instead. */
    bool home_gesture;

    /* Opt-in, like home_gesture above: NULL unless an app sets it. If
     * set, called only from console_screenshot_dump()
     * (CONFIG_LAUNCHER_DEVELOPMENT builds only) to let the running app
     * attach its own state to a screenshot capture - a JSON OBJECT fragment
     * (starting with `{`,
     * ending with `}`, no trailing comma) written into `out` (at most
     * `len` bytes, NUL-terminated). Spliced into the capture's
     * device-state JSON as a new "app" key. Diagnostic only - nothing
     * about the app's own behaviour depends on this. */
    void (*diagnostic_json)(char* out, size_t len);

    /* Opt-in, NULL unless an app declares one with APP_CONSOLE_PTR() above.
     * See docs/tools/Autana-CLI.md's "Adding a command from an app". */
    const app_console_t* console;
} app_t;

/*
 * The panel clock. Every app starts at the system value, the user's choice
 * kept across reboots. An app wanting another rate sets it with
 * gfx_set_panel_clock_hz(); the shell puts the system value back, and gfx
 * heal back to its defaults, whenever an app starts or exits, so no app
 * restores either.
 */
void shell_set_system_panel_clock_hz(int hz);
int shell_system_panel_clock_hz(void);

/*
 * Apps register themselves, so an app is entirely contained in
 * main/apps/<name>/ and deleting that folder removes it - source, logic and
 * tests - without touching another file, CMakeLists.txt included.
 *
 * APP_REGISTER() places a constructor in .init_array, which ESP-IDF runs
 * before app_main(), into a fixed array - no allocation, and registration
 * cannot fail at an awkward time. Link order decides .init_array order, so
 * the shell sorts by name before showing the list.
 */

#define APP_MAX 16

/* Called by APP_REGISTER before main(). Ignores anything past APP_MAX, having
 * complained about it. */
void app_register(const app_t* app);

#define APP_REGISTER(symbol)                                                                                           \
    __attribute__((constructor)) static void symbol##_register(void) { app_register(&symbol); }

/* Registered apps, sorted by name. Valid from the first line of app_main(). */
const app_t* const* app_list(void);
int app_list_count(void);
