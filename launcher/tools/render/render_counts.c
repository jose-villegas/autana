#include "render_counts.h"

#include "render/context/render_context.h"
#include "render/view/raster_cull_view.h"

FILE*
render_counts_open(const char* path, const char* tool) {
    FILE* rows = fopen(path, "w");
    if (rows == NULL) {
        fprintf(stderr, "%s: cannot write %s\n", tool, path);
        return NULL;
    }
#define COUNT_NAME(name, member) "," #name
    fprintf(rows, "frame,ms" RASTER_CULL_FRAME_FIELDS(COUNT_NAME) "\n");
#undef COUNT_NAME
    return rows;
}

void
render_counts_row(FILE* rows, const provider_frame_t* frame) {
    const raster_cull_frame_t f = raster_cull_frame(render_context_debug_view_state(render_context_main()));
    fprintf(rows, "%d,%u", frame->index, (unsigned)frame->elapsed_ms);
#define COUNT_VALUE(name, member) fprintf(rows, ",%d", f.member);
    RASTER_CULL_FRAME_FIELDS(COUNT_VALUE)
#undef COUNT_VALUE
    fputc('\n', rows);
}
