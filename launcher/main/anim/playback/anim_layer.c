#include "anim/playback/anim_layer.h"
#include <assert.h>
#include <string.h>

void
anim_layer_init(anim_layer_t* l, const anim_clip_t* clips, int count, anim_pose_t shown, anim_pose_t frozen) {
    assert(count >= 0 && count <= ANIM_LAYER_CLIP_NONE);
    assert(shown.joint_count == frozen.joint_count);
    assert(shown.joint_count == 0 || shown.local != frozen.local);
    *l = (anim_layer_t){.clips = clips, .clip_count = count, .shown = shown, .frozen = frozen};
    l->current.clip = ANIM_LAYER_CLIP_NONE;
    l->previous.clip = ANIM_LAYER_CLIP_NONE;
    anim_playhead_init(&l->current.playhead, ANIM_LOOP);
    anim_playhead_init(&l->previous.playhead, ANIM_LOOP);
}

void
anim_layer_play(anim_layer_t* l, uint8_t clip, uint32_t blend_ms) {
    assert(clip < l->clip_count);
    const bool interrupted = l->frozen_previous || l->previous.clip != ANIM_LAYER_CLIP_NONE;
    l->frozen_previous = interrupted && blend_ms != 0;
    if (l->frozen_previous) {
        memcpy(l->frozen.local, l->shown.local, (size_t)l->shown.joint_count * sizeof *l->shown.local);
    }
    l->previous = l->current;
    l->current.clip = clip;
    anim_playhead_init(&l->current.playhead, ANIM_LOOP);
    l->blend_ms = blend_ms;
    l->blended_ms = 0;
    if (blend_ms == 0 || l->frozen_previous) {
        l->previous.clip = ANIM_LAYER_CLIP_NONE;
    }
}

void
anim_layer_advance(anim_layer_t* l, uint32_t dt_ms) {
    if (l->current.clip == ANIM_LAYER_CLIP_NONE) {
        return;
    }
    anim_playhead_advance(&l->current.playhead, &l->clips[l->current.clip], dt_ms);
    if (!l->frozen_previous && l->previous.clip == ANIM_LAYER_CLIP_NONE) {
        return;
    }
    if (!l->frozen_previous) {
        anim_playhead_advance(&l->previous.playhead, &l->clips[l->previous.clip], dt_ms);
    }
    const uint32_t remaining = l->blend_ms - l->blended_ms;
    l->blended_ms += dt_ms > remaining ? remaining : dt_ms;
    if (l->blended_ms == l->blend_ms) {
        l->previous.clip = ANIM_LAYER_CLIP_NONE;
        l->frozen_previous = false;
    }
}

float
anim_layer_weight(const anim_layer_t* l) {
    return !l->frozen_previous && l->previous.clip == ANIM_LAYER_CLIP_NONE ? 1.0F
                                                                           : (float)l->blended_ms / (float)l->blend_ms;
}

static void
sample_slot(const anim_slot_t* slot, const anim_clip_t* clip, const anim_skeleton_t* skeleton,
            const anim_joint_bound_t* const* bound, const int* counts, anim_pose_t* out) {
    anim_pose_rest(skeleton, out);
    const float seconds = anim_playhead_sample_seconds(&slot->playhead, clip);
    anim_sample_pose(bound[slot->clip], counts[slot->clip], seconds, out);
}

const anim_pose_t*
anim_layer_pose(anim_layer_t* layer, const anim_skeleton_t* skeleton, const anim_joint_bound_t* const* bound,
                const int* counts) {
    assert(layer->current.clip != ANIM_LAYER_CLIP_NONE);
    sample_slot(&layer->current, &layer->clips[layer->current.clip], skeleton, bound, counts, &layer->shown);
    if (!layer->frozen_previous && layer->previous.clip != ANIM_LAYER_CLIP_NONE) {
        sample_slot(&layer->previous, &layer->clips[layer->previous.clip], skeleton, bound, counts, &layer->frozen);
    }
    if (layer->frozen_previous || layer->previous.clip != ANIM_LAYER_CLIP_NONE) {
        anim_pose_blend(&layer->frozen, &layer->shown, anim_layer_weight(layer), &layer->shown);
    }
    return &layer->shown;
}

static uint64_t
speed_integral(const anim_track_t* speed, float start, uint32_t t_ms) {
    uint32_t from = anim_playhead_key_ms(start);
    if (from >= t_ms) {
        return 0;
    }
    if (speed == NULL) {
        return (uint64_t)(t_ms - from) * ANIM_PLAYHEAD_SPEED_ONE;
    }
    anim_playhead_t rate;
    anim_playhead_init(&rate, ANIM_CLAMP);
    int key = anim_track_key_at(speed, start);
    uint64_t ticks = 0;
    while (from < t_ms) {
        if (key >= 0) {
            (void)anim_playhead_set_speed(&rate, speed->values[key]);
        }
        const uint32_t next = ++key < speed->count ? anim_playhead_key_ms(speed->times[key]) : t_ms;
        const uint32_t to = next < t_ms ? next : t_ms;
        ticks += (uint64_t)(to - from) * rate.speed_q16;
        from = to;
    }
    return ticks;
}

static void
sample_block_slot(const anim_blocks_t* blocks, const anim_clip_t* clips, int key, uint32_t t_ms, anim_slot_t* slot,
                  name_hash_t* name) {
    slot->clip = blocks->slots[key];
    (void)anim_name_decode(blocks->clip->values + key * ANIM_NAME_WIDTH, name);
    const uint64_t ticks = speed_integral(blocks->speed, blocks->clip->times[key], t_ms);
    anim_playhead_seek(&slot->playhead, &clips[slot->clip], ticks >> ANIM_PLAYHEAD_SPEED_SHIFT);
    slot->playhead.fraction = (uint16_t)(ticks & (ANIM_PLAYHEAD_SPEED_ONE - 1U));
}

anim_block_sample_t
anim_blocks_sample(const anim_blocks_t* blocks, const anim_clip_t* clips, anim_wrap_t wrap, uint32_t t_ms,
                   float seconds) {
    anim_block_sample_t sample = {.current.clip = ANIM_LAYER_CLIP_NONE, .previous.clip = ANIM_LAYER_CLIP_NONE};
    anim_playhead_init(&sample.current.playhead, wrap);
    anim_playhead_init(&sample.previous.playhead, wrap);
    const int key = anim_track_key_at(blocks->clip, seconds);
    if (key < 0) {
        return sample;
    }
    sample_block_slot(blocks, clips, key, t_ms, &sample.current, &sample.current_name);
    const float start = blocks->clip->times[key];
    const int blend_key = blocks->blend == NULL ? -1 : anim_track_key_at(blocks->blend, start);
    if (key > 0 && blend_key >= 0) {
        sample.blend_ms = (uint32_t)blocks->blend->values[blend_key];
        const uint32_t began = anim_playhead_key_ms(start);
        const uint32_t elapsed = t_ms > began ? t_ms - began : 0;
        sample.blended_ms = elapsed < sample.blend_ms ? elapsed : sample.blend_ms;
        if (sample.blended_ms < sample.blend_ms) {
            sample_block_slot(blocks, clips, key - 1, t_ms, &sample.previous, &sample.previous_name);
        }
    }
    return sample;
}

void
anim_layer_set(anim_layer_t* layer, const anim_block_sample_t* sample) {
    layer->current = sample->current;
    layer->previous = sample->previous;
    layer->blend_ms = sample->blend_ms;
    layer->blended_ms = sample->blended_ms;
    layer->frozen_previous = false;
}
