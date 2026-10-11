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
sample_slot(const anim_layer_t* layer, const anim_slot_t* slot, const anim_skeleton_t* skeleton,
            const anim_joint_bound_t* const* bound, const int* counts, anim_pose_t* out) {
    anim_pose_rest(skeleton, out);
    const float seconds = anim_playhead_seconds(&slot->playhead, &layer->clips[slot->clip]);
    anim_sample_pose(bound[slot->clip], counts[slot->clip], seconds, out);
}

const anim_pose_t*
anim_layer_pose(anim_layer_t* layer, const anim_skeleton_t* skeleton, const anim_joint_bound_t* const* bound,
                const int* counts) {
    assert(layer->current.clip != ANIM_LAYER_CLIP_NONE);
    sample_slot(layer, &layer->current, skeleton, bound, counts, &layer->shown);
    if (!layer->frozen_previous && layer->previous.clip != ANIM_LAYER_CLIP_NONE) {
        sample_slot(layer, &layer->previous, skeleton, bound, counts, &layer->frozen);
    }
    if (layer->frozen_previous || layer->previous.clip != ANIM_LAYER_CLIP_NONE) {
        anim_pose_blend(&layer->frozen, &layer->shown, anim_layer_weight(layer), &layer->shown);
    }
    return &layer->shown;
}
