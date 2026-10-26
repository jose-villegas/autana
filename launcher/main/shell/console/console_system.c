/* Console borrows app sound storage until the shell's mixer fence has passed. */
#include "shell/shell_system.h"
#include "sound/console/console_sound.h"

static shell_system_t console_system = {
    .name = "console",
    .order = SHELL_ORDER_CONSOLE,
    .app_exit = console_sound_release_views,
};

SHELL_SYSTEM_REGISTER(console_system);
