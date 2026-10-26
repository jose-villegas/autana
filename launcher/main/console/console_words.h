/*
 * console_words: the words of a console line and the one-line reply, shared
 * by the console's verbs, the shell's app dispatch and an app's handler.
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
