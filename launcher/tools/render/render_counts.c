#include "render_counts.h"

#include <stdlib.h>
#include <string.h>

#include "render/context/render_context.h"
#include "render/path/r3d_path.h"
#include "render/view/raster_cull_view.h"
#include "scene/scene_internal.h"
#include "scene/transform/transform.h"

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

static void
pose_string(FILE* out, const char* value) {
    fputc('"', out);
    for (const unsigned char* p = (const unsigned char*)value; *p != 0; p++) {
        if (*p == '"' || *p == '\\') {
            fputc('\\', out);
        }
        if (*p < ' ') {
            fprintf(out, "\\u%04x", (unsigned)*p);
        } else {
            fputc(*p, out);
        }
    }
    fputc('"', out);
}

static void
pose_matrix(FILE* out, entity_t entity) {
    const mat4f_t world = transform_world_matrix(entity);
    const size_t axes = sizeof world.m / sizeof world.m[0];
    fputc('[', out);
    for (size_t row = 0; row < axes; row++) {
        fprintf(out, "%s[", row == 0 ? "" : ",");
        for (size_t col = 0; col < axes; col++) {
            fprintf(out, "%s%.9g", col == 0 ? "" : ",", (double)world.m[row][col]);
        }
        fputc(']', out);
    }
    fputc(']', out);
}

void
render_pose_row(FILE** rows, const provider_frame_t* frame) {
    FILE* out = *rows;
    fprintf(out, "%s{\"frame\":%d,\"camera\":", frame->index == 0 ? "" : ",\n", frame->index);
    const entity_t camera = camera_active();
    const lens_t* lens = camera_lens(camera);
    if (lens == NULL) {
        fprintf(out, "null");
    } else {
        fprintf(out, "{\"world\":");
        pose_matrix(out, camera);
        fprintf(out, ",\"lens\":%.9g,\"near\":%.9g,\"shift\":[%.9g,%.9g]}", (double)lens->half_fov_short_tan,
                (double)lens->near_z, (double)lens->shift.x, (double)lens->shift.y);
    }
    fprintf(out, ",\"skins\":[");
    bool first = true;
    for (int root = 0; root < SCENE_ROOTS_MAX; root++) {
        const scene_t* scene = scene_ready_at(root);
        if (scene == NULL) {
            continue;
        }
        for (int row = 0; row < scene->asset.entity_count; row++) {
            if (!scene->skinned_renderers[row].bound) {
                continue;
            }
            const entity_t entity = scene_row_entity(scene, (scene_row_t)row);
            const anim_layer_t* layer = &scene->animators[row].layer;
            fprintf(out, "%s{\"name\":", first ? "" : ",");
            first = false;
            pose_string(out, entity_name(entity));
            fprintf(out, ",\"clip\":");
            const char* clip = animator_clip_name(entity, animator_current_clip(entity));
            if (clip == NULL) {
                fprintf(out, "null");
            } else {
                pose_string(out, clip);
            }
            fprintf(out, ",\"time\":%.9g,\"blend\":%.9g,", (double)anim_playhead_seconds(&layer->current.playhead),
                    (double)anim_layer_weight(layer));
            fprintf(out, "\"frozen_previous\":%s,\"world\":", layer->frozen_previous ? "true" : "false");
            pose_matrix(out, entity);
            fputc('}', out);
        }
    }
    fprintf(out, "]}");
    bool ok = !ferror(out);
    if (frame->index + 1 == frame->count) {
        fprintf(out, "\n]\n");
        ok = !ferror(out) && ok;
        ok = fclose(out) == 0 && ok;
        *rows = NULL;
    }
    if (!ok) {
        fprintf(stderr, "render pose JSON: write failed\n");
        exit(EXIT_FAILURE);
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
