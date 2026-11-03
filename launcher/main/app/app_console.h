/*
 * app_console: an app's console command. An app parses its own subcommands
 * with the words in console/console_words.h.
 */
#pragma once

#include <stdbool.h>

#include "console/console_words.h"

/* An app's own console command: docs/cli/App-Commands.md. The shell matches `prefix` as a whole word
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
