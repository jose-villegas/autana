#include "render/raster_split.h"

void
raster_split_forget(raster_split_t* split) {
    *split = (raster_split_t){0};
}

void
raster_split_fold_hidden(raster_split_t* split, const raster_split_hidden_tally_t* a,
                         const raster_split_hidden_tally_t* b) {
    raster_split_hidden_rates_t* rates = &split->hidden;
    rates->probe_phase = (rates->probe_phase + 1U) % RASTER_SPLIT_HIDDEN_PROBE_EVERY;
    const uint32_t tested = a->tested + b->tested;
    if (tested != 0) {
        /* Count clusters equally. Their sizes already inform each candidate's saving. */
        rates->hidden_rate = (float)(a->skipped + b->skipped) / (float)tested;
        rates->scan_rate =
            (float)((uint64_t)a->pixels_read + b->pixels_read) / (float)((uint64_t)a->box_pixels + b->box_pixels);
        rates->valid = true;
    }
}
