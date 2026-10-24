/* Console: answer status under the app prefix. */
#include <stdio.h>
#include <string.h>

#include "app/app.h"

static bool
app_example_console(const char* args) {
    if (strcmp(args, "status") != 0) {
        return false;
    }
    printf("EXAMPLE status=ok\n");
    printf("EXAMPLE_END\n");
    return true;
}

APP_CONSOLE("example", app_example_console);

app_t app_example = {
    .name = "Example",
    .console = APP_CONSOLE_PTR(app_example_console),
};
