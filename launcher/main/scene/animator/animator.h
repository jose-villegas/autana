/*
 * animator: plays an entity's clips. Skeleton clips pose its skinned
 * renderer; property clips animate scene components by name.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "scene/scene.h"

typedef enum { ANIMATOR_CURRENT, ANIMATOR_PREVIOUS } animator_slot_t;

/* skeleton_id NULL binds property clips to scene components. Asset ids are resolved once. */
bool animator_set_clips(entity_t entity, const char* skeleton_id, const char* const* clips, int count,
                        scene_failure_t* why);
/* Property clips require blend_ms == 0. Skeleton clips support a transition blend. */
bool animator_play(entity_t entity, int clip, uint32_t blend_ms);
bool animator_set_speed(entity_t entity, animator_slot_t slot, float speed);
bool animator_set_paused(entity_t entity, animator_slot_t slot, bool paused);
/* Samples the scene systems at once, without drawing. */
bool animator_seek(entity_t entity, animator_slot_t slot, uint32_t t_ms);

int animator_clip_count(entity_t entity);
const char* animator_clip_name(entity_t entity, int clip);
uint32_t animator_clip_duration_ms(entity_t entity, int clip);
/* The clip the current slot plays, or -1 when none does. */
int animator_current_clip(entity_t entity);
