/* Give microui a hover root on the frame a press first lands. */
#pragma once

#include "microui.h"

/* Call after the pointer's first position is fed and before mu_begin().
 *
 * mu_begin() takes hover_root from the previous frame's next_hover_root, so
 * a finger that arrives from the parked pointer has no hover root until the
 * frame after, and its DOWN would wait a whole extra frame. The containers
 * of the last frame are still in root_list, so the root the new position
 * lands in can be answered now, by the rule begin_root_container() uses. */
void ui_hover_seed_root(mu_Context* ctx);
