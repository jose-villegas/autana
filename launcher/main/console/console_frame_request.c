/* console_frame_request: see console_frame_request.h. */
#include "console/console_frame_request.h"

#include <string.h>

static console_frame_mailbox_t shared = CONSOLE_FRAME_MAILBOX_INIT;

console_frame_mailbox_t*
console_frame_mailbox(void) {
    return &shared;
}

static void
merge(console_frame_request_t* into, const console_frame_request_t* from) {
    if (from->kinds & CONSOLE_FRAME_NAVIGATE) {
        into->navigation = from->navigation;
        memcpy(into->app, from->app, sizeof into->app);
    }
    if (from->kinds & CONSOLE_FRAME_FREEZE) {
        into->frozen = from->frozen;
        into->steps = from->steps;
    }
#ifdef CONSOLE_FRAME_HAS_RUNSUITE
    if (from->kinds & CONSOLE_FRAME_RUNSUITE) {
        memcpy(into->suite, from->suite, sizeof into->suite);
    }
#endif
    into->kinds |= from->kinds;
}

bool
console_frame_post(console_frame_mailbox_t* mailbox, const console_frame_request_t* request) {
    portENTER_CRITICAL(&mailbox->lock);
    const uint32_t held = (mailbox->pending.kinds | mailbox->busy) & CONSOLE_FRAME_HELD;
    const bool accepted = (request->kinds & held) == 0;
    if (accepted) {
        merge(&mailbox->pending, request);
    }
    portEXIT_CRITICAL(&mailbox->lock);
    return accepted;
}

void
console_frame_take(console_frame_mailbox_t* mailbox, console_frame_request_t* out) {
    portENTER_CRITICAL(&mailbox->lock);
    *out = mailbox->pending;
    mailbox->busy |= out->kinds & CONSOLE_FRAME_HELD;
    mailbox->pending.kinds = 0;
    portEXIT_CRITICAL(&mailbox->lock);
}

void
console_frame_done(console_frame_mailbox_t* mailbox, console_frame_kind_t kind) {
    portENTER_CRITICAL(&mailbox->lock);
    mailbox->busy &= ~(uint32_t)kind;
    portEXIT_CRITICAL(&mailbox->lock);
}
