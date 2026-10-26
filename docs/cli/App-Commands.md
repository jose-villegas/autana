# App commands

An app answers its own console commands without joining the console's verb
list, whose verbs each live in a `console/` folder beside the module they drive
(`launcher/main/sound/console/` for the sound verbs):

<!-- generated: app-console-example check: python launcher/tools/gen/examples_doc.py --check -->
```c
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
```
<!-- /generated: app-console-example -->

- `APP_CONSOLE()` declares the prefix once, before the `app_t`. A prefix that
  clashes with a verb or another app's fails the boot.
- The handler gets what follows the prefix: `autana example status` (or
  `example status` in a session) calls it with `"status"`.
- Every reply line starts with the prefix in capitals; the last is
  `<PREFIX>_END`, which lets autana return at once.
- Return `false` to decline; the shell answers `<PREFIX>_ERR not handled`. An
  app that is not running gets `<PREFIX>_ERR not running`.
- It runs on the frame loop after the app's `frame()`, and while frozen. Lines
  faster than frames queue up to `APP_LINE_QUEUE_LEN`; one past that is dropped
  with no reply.
- No `#if` needed: `APP_CONSOLE_PTR()` is `NULL` in release and the handler
  goes with it.

Apps with switchable scenes answer `scenes` with one
`<PREFIX> scene=<key> ...` line per scene, followed by `<PREFIX>_END`.
`scene <key>` selects that key and ends with `<PREFIX>_END` or reports
`<PREFIX>_ERR <reason>`. Keys are unique, nonempty words. An app without
switchable scenes may decline `scenes` with `<PREFIX>_ERR not handled`.

The development console's `APPS` reply lists
`APPS name=<name> running=<0|1>`, appending `console=<prefix>` when the
app declares `.console`; `APPS_END` terminates the list. The prefix comes
from that app's `APP_CONSOLE()` declaration. `autana apps --json` includes
it as the optional `console` field.

An app that adds a command documents it itself.
