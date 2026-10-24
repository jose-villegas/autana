/* Track: load a position curve once, then sample it each frame. */
#include "anim/anim_tracks.h"
#include "anim/playback/anim_playhead.h"

typedef struct {
    anim_tracks_t clip;
    anim_track_t move;
    anim_playhead_t playhead;
    float out[ANIM_WIDTH_MAX];
} track_example_t;

static bool
track_example_start(track_example_t* s, const asset_pack_t* pack, const char* clip_id, const char* node) {
    if (anim_tracks_from_pack(pack, clip_id, &s->clip) != ASSET_OK
        || anim_tracks_find(&s->clip, name_hash(node), ANIM_COMPONENT_TRANSFORM, name_hash("position"), &s->move)
               != ASSET_OK) {
        return false;
    }
    anim_playhead_init(&s->playhead, ANIM_LOOP);
    return true;
}

static void
track_example(track_example_t* s, uint32_t dt_ms) {
    anim_playhead_advance(&s->playhead, &s->clip.clip, dt_ms);
    anim_track_sample(&s->move, anim_playhead_seconds(&s->playhead), s->out);
}
