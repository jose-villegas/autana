/*
 * render_watch - the frame watch (util/frame_watch.h) over a host render:
 * every scene frame's allocations, frees and stdout writes, judged by the
 * same rule the board uses, so a scene whose steady state allocates or
 * prints fails its render.
 *
 * The heap is watched through the link (-Wl,--wrap=malloc and its three
 * siblings, render_scene.sh), which sees each caller's address. stdout is
 * watched by capturing it in a file and measuring it per frame: ESP_LOG*
 * prints there on a host, and so does a plain printf(), with no call site
 * to hook on toolchains whose printf() is inline.
 */
#ifndef RENDER_WATCH_H
#define RENDER_WATCH_H

#include <stdbool.h>

/* `binary` is named in each warning's addr2line hint. A `console_path`
 * sends stdout into that file until render_watch_finish(); NULL leaves
 * stdout alone and unwatched. False, with stdout untouched, when the file
 * cannot be made. */
bool render_watch_start(const char* binary, const char* console_path);

void render_watch_frame_begin(void);

/* Closes the frame, printing a FRAME_WATCH line to stderr for every site
 * that has just become repeating. */
void render_watch_frame_end(void);

/* Gives stdout back, with what it captured, and returns how many sites
 * became repeating. */
int render_watch_finish(void);

#endif
