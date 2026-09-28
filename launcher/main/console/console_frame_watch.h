/*
 * console_frame_watch - FRAMEWATCH: the frame watch's counts and repeating
 * sites (util/frame_watch.h) as one `FRAMEWATCH <json>` line. The verb only
 * sets a latch; the frame loop answers, since the counts are its own.
 * Development builds only - see console.h.
 */
#pragma once

/* Answers a FRAMEWATCH seen since the last call, if there was one. */
void console_frame_watch_answer(void);
