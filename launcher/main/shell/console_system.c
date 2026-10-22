/* Console borrows app sound storage until the shell's mixer fence has passed. */
#include "console/verbs/console_sound.h"
#include "shell/shell_system.h"

static shell_system_t console_system = {
    .name = "console",
    .order = SHELL_ORDER_CONSOLE,
    .app_exit = console_sound_app_exit,
};

SHELL_SYSTEM_REGISTER(console_system);
