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
track_example_start(track_example_t* s, const asset_pack_t* pack) {
    const name_hash_t camera = name_hash("camera");
    const name_hash_t position = name_hash("position");
    if (anim_tracks_from_pack(pack, "probe", &s->clip) != ASSET_OK
        || anim_tracks_find(&s->clip, camera, ANIM_COMPONENT_TRANSFORM, position, &s->move) != ASSET_OK) {
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
