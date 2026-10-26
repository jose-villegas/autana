/* console_frame_request: see console_frame_request.h. */
#include "console/console_frame_request.h"

#include <string.h>

#include "core/registry.h"

static console_frame_slots_t shared = CONSOLE_FRAME_SLOTS_INIT;

console_frame_slots_t*
console_frame_slots(void) {
    return &shared;
}

bool
console_frame_add(console_frame_slots_t* slots, console_frame_slot_t* slot) {
    static const registry_rule_t rule = REGISTRY_RULE(console_frame_slot_t, strcmp, NULL);
    return registry_add(&slots->first, &rule, slot) != REGISTRY_CLASH;
}

bool
console_frame_post(console_frame_slots_t* slots, console_frame_slot_t* slot, const void* payload) {
    portENTER_CRITICAL(&slots->lock);
    const bool accepted = !(slot->held && (slot->posted || slot->busy));
    if (accepted) {
        if (slot->size != 0) {
            memcpy(slot->pending, payload, slot->size);
        }
        slot->posted = true;
    }
    portEXIT_CRITICAL(&slots->lock);
    return accepted;
}

bool
console_frame_take(console_frame_slots_t* slots, console_frame_slot_t* slot) {
    portENTER_CRITICAL(&slots->lock);
    const bool posted = slot->posted;
    if (posted) {
        if (slot->size != 0) {
            memcpy(slot->taken, slot->pending, slot->size);
        }
        slot->posted = false;
        slot->busy = slot->held;
    }
    portEXIT_CRITICAL(&slots->lock);
    return posted;
}

void
console_frame_done(console_frame_slots_t* slots, console_frame_slot_t* slot) {
    portENTER_CRITICAL(&slots->lock);
    slot->busy = false;
    portEXIT_CRITICAL(&slots->lock);
}

void
console_frame_answer_pending(console_frame_slots_t* slots) {
    for (console_frame_slot_t* slot = slots->first; slot != NULL; slot = slot->next) {
        if (slot->answer != NULL && console_frame_take(slots, slot)) {
            slot->answer(slot->taken);
            console_frame_done(slots, slot);
        }
    }
}
