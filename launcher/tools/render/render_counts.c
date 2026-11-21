#include "render_counts.h"

#include <string.h>

#include "render/context/render_context.h"
#include "render/path/r3d_path.h"
#include "render/view/raster_cull_view.h"

FILE*
render_counts_open(const char* path, const char* tool) {
#define COUNT_NAME(name, member) "," #name
    return render_rows_open(path, "frame,ms" RASTER_CULL_FRAME_FIELDS(COUNT_NAME), tool);
#undef COUNT_NAME
}

void
render_counts_row(FILE** rows, const provider_frame_t* frame) {
    raster_cull_frame_t f;
    raster_cull_frame(render_context_debug_view_state(render_context_main()), &f);
    fprintf(*rows, "%d,%u", frame->index, (unsigned)frame->elapsed_ms);
#define COUNT_VALUE(name, member) fprintf(*rows, ",%d", f.member);
    RASTER_CULL_FRAME_FIELDS(COUNT_VALUE)
#undef COUNT_VALUE
    fputc('\n', *rows);
    if (frame->index + 1 == frame->count) {
        (void)fclose(*rows);
        *rows = NULL;
    }
}

bool
render_path_hits_write(const char* path, const char* tool) {
    FILE* rows = render_rows_open(path, "path,hits", tool);
    if (rows == NULL) {
        return false;
    }
    for (const r3d_path_t* p = r3d_path_first(); p != NULL;) {
        const char* name = p->name;
        uint64_t hits = 0;
        for (; p != NULL && strcmp(p->name, name) == 0; p = p->next) {
            hits += p->hits;
        }
        fprintf(rows, "%s,%llu\n", name, (unsigned long long)hits);
    }
    if (fclose(rows) != 0) {
        fprintf(stderr, "%s: cannot write %s\n", tool, path);
        return false;
    }
    return true;
}
