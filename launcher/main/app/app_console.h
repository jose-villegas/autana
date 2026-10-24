/*
 * app_console: an app's console command, and the words of a console line,
 * shared by the console's own verbs, the shell's app dispatch and an app's
 * handler, so an app parses its own subcommands without reaching into
 * console/.
 *
 * Words match ignoring ASCII case: the lowercase word a person types and the
 * uppercase one a harness sends are the same word.
 */
#pragma once

#include <stdbool.h>

typedef void (*console_reply_fn)(const char* line);

/* printf()+fflush() for a one-line reply. */
void console_reply_stdio(const char* line);

/* strcmp() with ASCII case folded. */
int console_name_cmp(const char* a, const char* b);

/* True if `line` is `name`, or `name` plus one space and more (never a
 * longer word starting the same way). `*args` gets what follows: "" for a
 * bare match. */
bool console_word_match(const char* line, const char* name, const char** args);

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
