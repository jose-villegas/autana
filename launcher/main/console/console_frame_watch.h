/*
 * console_frame_watch (FRAMEWATCH): the frame watch's counts and repeating
 * sites (profile/frame_watch.h) as one `FRAMEWATCH <json>` line. The verb only
 * posts a frame request; the frame loop answers, since the counts are its own.
 * Development builds only; see console.h.
 */
#pragma once

/* Answers a FRAMEWATCH the frame loop took. */
void console_frame_watch_answer(void);
