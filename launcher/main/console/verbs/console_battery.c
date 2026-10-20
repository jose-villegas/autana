/* Battery reads block on the console task, outside the frame loop. */
#include <ctype.h>
#include "console/console_verbs.h"
#include "console/verbs/console_battery_reply.h"

static void
log_reply(const power_log_record_t* record, void* context) {
    const console_reply_fn reply = *(console_reply_fn*)context;
    char line[CONSOLE_BATTERY_REPLY_MAX];
    console_battery_log_reply(line, sizeof line, record);
    reply(line);
}

static void
console_verb_battery(const char* args, console_reply_fn reply) {
    const console_battery_action_t action = console_battery_parse(args);
    switch (action) {
        case CONSOLE_BATTERY_LOG: {
            const int count = power_log_dump(log_reply, &reply);
            char line[CONSOLE_BATTERY_REPLY_MAX];
            if (count < 0) {
                reply("BATLOG error read");
            } else {
                (void)snprintf(line, sizeof line, "BATLOG end count=%d", count);
                reply(line);
            }
            return;
        }
        case CONSOLE_BATTERY_CLEAR: reply(power_log_clear() ? "BATLOG cleared" : "BATLOG error clear"); return;
        case CONSOLE_BATTERY_INVALID: reply("BATTERY usage: battery [LOG [CLEAR]]"); return;
        default: break;
    }
    power_battery_t battery;
    power_registers_t regs;
    (void)power_read_battery_now(&battery, &regs);
    char line[CONSOLE_BATTERY_REPLY_MAX];
    console_battery_reply(line, sizeof line, &battery, &regs);
    reply(line);
}
CONSOLE_VERB(battery, 0, console_verb_battery)
