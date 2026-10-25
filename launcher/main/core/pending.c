#include "core/pending.h"
#include <stddef.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_LAUNCHER_DEVELOPMENT
#include "esp_log.h"
#endif

typedef struct {
    uint16_t generation;
    bool alive;
} token_t;

_Static_assert(PENDING_TOKENS_MAX <= HANDLE_SLOT_MAX, "the dead owner must stay outside the token pool");

static token_t tokens[PENDING_TOKENS_MAX];

static token_t*
find_token(handle_t token) {
    const uint32_t slot = handle_slot(token);
    if (handle_owner(token) != HANDLE_OWNER_TOKEN || slot >= PENDING_TOKENS_MAX || handle_generation(token) == 0
        || tokens[slot].generation != handle_generation(token)) {
        return NULL;
    }
    return &tokens[slot];
}

handle_t
pending_token_new(void) {
    for (uint32_t i = 0; i < PENDING_TOKENS_MAX; i++) {
        token_t* token = &tokens[i];
        if (!token->alive && handle_generation_next(token->generation)) {
            token->generation++;
            token->alive = true;
            return handle_make(HANDLE_OWNER_TOKEN, i, token->generation);
        }
    }
#if CONFIG_LAUNCHER_DEVELOPMENT
    ESP_LOGW("pending", "token pool exhausted");
#endif
    return PENDING_OWNER_DEAD;
}

void
pending_token_cancel(handle_t token) {
    token_t* found = find_token(token);
    if (found != NULL) {
        found->alive = false;
    }
}

void
pending_tokens_cancel(void) {
    for (uint32_t i = 0; i < PENDING_TOKENS_MAX; i++) {
        tokens[i].alive = false;
    }
}

bool
pending_owner_alive(handle_t owner) {
    if (handle_is_none(owner)) {
        return true;
    }
    switch (handle_owner(owner)) {
        case HANDLE_OWNER_TOKEN: {
            const token_t* token = find_token(owner);
            return token != NULL && token->alive;
        }
        default: return false;
    }
}
