/* shell_frame: the shell chrome shared by raw drawing and expanded-strip replay. */
#pragma once

void shell_frame_init(const char* short_id);
void shell_frame_overlay(int row0, int row1);
void shell_frame_extras(void);
