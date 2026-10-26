/* pending: completion states and cancellation groups, used only on core 0. */
#pragma once
#include "core/handle.h"

/* Live cancellation groups at once; a full pool hands out PENDING_OWNER_DEAD and logs. */
#define PENDING_TOKENS_MAX 4U
#define PENDING_OWNER_DEAD (handle_make(HANDLE_OWNER_TOKEN, HANDLE_SLOT_MAX, HANDLE_GENERATION_MAX))

typedef enum { PENDING_NONE, PENDING_WAIT, PENDING_READY, PENDING_FAILED, PENDING_CANCELLED } pending_t;

typedef void (*pending_fn)(handle_t request, pending_t state, handle_t owner);

/* Exhaustion returns PENDING_OWNER_DEAD; tokens end with the current app. */
handle_t pending_token_new(void);
void pending_token_cancel(handle_t token);
void pending_tokens_cancel(void);
/* HANDLE_NONE is the default owner, ended when the frame loop ends the current app;
 * unknown and stale owners are dead. */
bool pending_owner_alive(handle_t owner);
