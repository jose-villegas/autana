/* pending_system: cancellation-group lifetime at the app boundary. */
#include "core/pending.h"
#include "shell/shell_system.h"

static shell_system_t pending_system = {
    .name = "pending",
    .order = SHELL_ORDER_PENDING,
    .app_exit = pending_tokens_cancel,
};
SHELL_SYSTEM_REGISTER(pending_system);
