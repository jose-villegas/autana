#include "render/raster_split.h"

void
raster_split_forget(raster_split_t* split) {
    *split = (raster_split_t){0};
}
