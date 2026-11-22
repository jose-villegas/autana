/* Start playback and seek to the midpoint before pausing on that frame. */
#include "scene/timeline/timeline.h"

static uint32_t
preview_timeline(entity_t scene) {
    timeline_play(scene);
    timeline_seek(scene, timeline_duration_ms(scene) / 2);
    timeline_pause(scene);
    return timeline_time_ms(scene);
}
