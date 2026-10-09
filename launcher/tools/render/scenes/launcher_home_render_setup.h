/* Launcher scene inputs shared by home and boot host renders. */
#pragma once

#include <stdbool.h>

bool launcher_home_render_add_row(const char* name);
void launcher_home_render_register_rows(void);
void launcher_home_render_setup(int quarter);
