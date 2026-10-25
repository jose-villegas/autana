/* pending: completion states and cancellation groups, used only on core 0. */
#pragma once
#include "core/handle.h"

#define PENDING_TOKENS_MAX 16U

typedef enum { PENDING_NONE, PENDING_WAIT, PENDING_READY, PENDING_FAILED, PENDING_CANCELLED } pending_t;

typedef void (*pending_fn)(handle_t request, pending_t state, handle_t owner);

handle_t pending_token_new(void);
void pending_token_cancel(handle_t token);
/* HANDLE_NONE denotes the running app; unknown and stale owners are dead. */
bool pending_owner_alive(handle_t owner);
