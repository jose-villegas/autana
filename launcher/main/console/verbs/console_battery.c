/* Battery reads block on the console task, outside the frame loop. */
#include <ctype.h>
#include "console/console_verbs.h"
#include "console/verbs/console_battery_reply.h"

static void
console_verb_battery(const char* args, console_reply_fn reply) {
    while (isspace((unsigned char)*args)) {
        args++;
    }
    if (*args) {
        reply("BATTERY usage: battery");
        return;
    }
    power_battery_t battery;
    power_registers_t regs;
    (void)power_read_battery_raw(&battery, &regs);
    char line[CONSOLE_BATTERY_REPLY_MAX];
    console_battery_reply(line, sizeof line, &battery, &regs);
    reply(line);
}
CONSOLE_VERB(battery, 0, console_verb_battery)
