/* timeline: controls the clip on a scene root's millisecond clock. */
#pragma once

#include "scene/entity/entity.h"

void timeline_play(entity_t root);
void timeline_pause(entity_t root);
bool timeline_playing(entity_t root);
/* Applies the addressed frame and re-poses immediately, including while paused. */
void timeline_seek(entity_t root, uint32_t t_ms);
uint32_t timeline_time_ms(entity_t root);
uint32_t timeline_duration_ms(entity_t root);
