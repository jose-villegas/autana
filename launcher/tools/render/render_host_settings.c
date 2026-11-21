/* The renderer's halves of render_host.c: --set render.FIELD=value reaches
 * the main render context as an app's pack sets it, and --poses reads the
 * active scene camera. */

#include "render_host.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "render/batch/r3d_batch.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/raster_internal.h"
#include "scene/animator/animator.h"
#include "scene/camera/camera.h"
#include "scene/transform/transform.h"
#include "ui/layout/ui_transform.h"

bool
render_host_apply_settings(const render_settings_t* settings) {
    render_context_set_settings(render_context_main(), settings);
    return true;
}

bool
render_host_camera_pose(float position[3], float rotation[4]) {
    const entity_t camera = camera_active();
    if (handle_is_none(camera)) {
        return false;
    }
    const transformf_t world = transform_world(camera);
    position[0] = world.position.x;
    position[1] = world.position.y;
    position[2] = world.position.z;
    rotation[0] = world.rotation.x;
    rotation[1] = world.rotation.y;
    rotation[2] = world.rotation.z;
    rotation[3] = world.rotation.w;
    return true;
}

bool
render_host_camera_input(const float eye[3], const float forward[3], float lens, float near_z) {
    const entity_t camera = camera_active();
    if (handle_is_none(camera)) {
        return false;
    }
    (void)animator_set_paused(camera, ANIMATOR_CURRENT, true);
    (void)animator_set_paused(camera, ANIMATOR_PREVIOUS, true);
    if (!camera_set_projection(camera, lens, near_z)) {
        return false;
    }
    transformf_t pose = transform_world(camera);
    transformf_set_position(&pose, (vec3f_t){eye[0], eye[1], -eye[2]});
    transformf_look_along(&pose, (vec3f_t){forward[0], forward[1], -forward[2]}, vec3f_unit_y());
    transform_set_world(camera, pose);
    camera_cut(camera);
    return true;
}

#define ID_FILE_SUFFIX       "/0000.ids"
#define ID_WORD_BYTES        sizeof(uint32_t)
#define ID_AXES              3
#define JSON_PRINTABLE_FIRST 0x20

static bool id_entries_written;

static bool
write_id_pixels(const char* directory, int frame, const image_t* image, affine2x_t picture_to_panel, int width,
                int height) {
    const size_t size = strlen(directory) + sizeof ID_FILE_SUFFIX + sizeof(int) * CHAR_BIT;
    char* path = malloc(size);
    if (path == NULL) {
        return false;
    }
    (void)snprintf(path, size, "%s/%04d.ids", directory, frame);
    FILE* file = fopen(path, "wb");
    free(path);
    if (file == NULL) {
        return false;
    }
    bool ok = true;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const recti_t pixel = ui_transform_rect(picture_to_panel, (recti_t){x, y, 1, 1});
            const uint8_t* row = (const uint8_t*)image->pixels + pixel.y * image->pitch;
            const uint32_t value =
                image->format == IMAGE_U32 ? ((const uint32_t*)row)[pixel.x] : ((const uint16_t*)row)[pixel.x];
            uint8_t bytes[ID_WORD_BYTES];
            for (size_t b = 0; b < ID_WORD_BYTES; b++) {
                bytes[b] = (uint8_t)(value >> (b * CHAR_BIT));
            }
            ok &= fwrite(bytes, sizeof bytes, 1, file) == 1;
        }
    }
    return fclose(file) == 0 && ok;
}

static void
write_json_name(FILE* file, const char* name) {
    (void)fputc('"', file);
    for (const unsigned char* at = (const unsigned char*)name; *at != '\0'; at++) {
        if (*at == '"' || *at == '\\') {
            (void)fputc('\\', file);
            (void)fputc(*at, file);
        } else if (*at < JSON_PRINTABLE_FIRST) {
            (void)fprintf(file, "\\u%04x", (unsigned)*at);
        } else {
            (void)fputc(*at, file);
        }
    }
    (void)fputc('"', file);
}

static bool
write_id_entry(FILE* file, const raster_t* raster, int entry, const r3d_span_writer_t* writer, int frame) {
    const r3d_instance_t* instance = raster_drawn_at(raster, entry);
    asset_location_t location;
    if (!asset_store_locate(instance->mesh->triangles, &location)) {
        return false;
    }
    const handle_t load = asset_store_load(location.pack_name);
    const asset_pack_t* pack = asset_store_loaded(load);
    asset_entry_t asset;
    if (pack == NULL || asset_pack_entry(pack, location.entry, &asset) != ASSET_OK) {
        asset_store_release(load);
        return false;
    }
    const int count = writer->step == R3D_SPAN_STEP_TRIANGLE  ? instance->mesh->triangle_count
                      : writer->step == R3D_SPAN_STEP_CLUSTER ? instance->mesh->cluster_count
                                                              : 1;
    const char* step = writer->step == R3D_SPAN_STEP_TRIANGLE  ? "triangle"
                       : writer->step == R3D_SPAN_STEP_CLUSTER ? "cluster"
                                                               : "none";
    (void)fprintf(file, "%s{\"frame\":%d,\"entry\":", id_entries_written ? "," : "", frame);
    write_json_name(file, asset.name);
    (void)fprintf(file, ",\"first_id\":%u,\"id_count\":%d,\"triangle_count\":%d,\"step\":\"%s\",\"matrix\":[",
                  (unsigned)writer->value, count, instance->mesh->triangle_count, step);
    asset_store_release(load);
    const r3d_batch_draw_t* copy = raster_drawn_copy(raster, entry);
    mat4f_t model = copy != NULL ? r3d_batch_copy_model(copy->batch, copy->copy) : mat4f_identity();
    if (instance->model != NULL) {
        mat4f_pre_mul_affine(instance->model, &model);
    }
    for (int r = 0; r < ID_AXES; r++) {
        (void)fprintf(file, "%s[", r == 0 ? "" : ",");
        for (int c = 0; c < ID_AXES; c++) {
            (void)fprintf(file, "%s%.9g", c == 0 ? "" : ",", (double)model.m[r][c]);
        }
        (void)fputs("]", file);
    }
    (void)fputs("],\"position\":[", file);
    for (int r = 0; r < ID_AXES; r++) {
        (void)fprintf(file, "%s%.9g", r == 0 ? "" : ",", (double)model.m[r][ID_AXES]);
    }
    id_entries_written = true;
    return fputs("]}", file) >= 0;
}

bool
render_host_write_ids(const char* directory, FILE* entries, int frame, int quarter, bool panel) {
    render_context_t* context = render_context_main();
    const raster_t* raster = &context->raster;
    if (raster->attachment_count != 1 || raster->scratch == NULL) {
        return false;
    }
    const raster_attachment_t* attachment = raster->attachments[0];
    if (attachment->writer == NULL || (attachment->format != IMAGE_U16 && attachment->format != IMAGE_U32)) {
        return false;
    }
    if (attachment->begin != NULL) {
        attachment->begin(attachment, raster, &context->frame_view);
    }
    int width, height;
    const affine2x_t panel_to_picture =
        render_host_picture_transform(quarter, panel, raster->width, raster->height, &width, &height);
    affine2x_t picture_to_panel;
    if (!affine2x_invert(panel_to_picture, &picture_to_panel)) {
        return false;
    }
    if (frame == 0) {
        const render_view_t* view = &context->frame_view;
        id_entries_written = false;
        (void)fprintf(entries, "{\"width\":%d,\"height\":%d,\"lens\":%.9g,\"near\":%.9g,\"entries\":[", width, height,
                      (double)render_view_half_fov_short_tan(view), (double)view->lens.near_z);
    }
    for (int e = 0; e < raster_drawn_count(raster); e++) {
        r3d_span_writer_t writer = {0};
        if (!attachment->writer(attachment, e, &writer) || writer.span != raster_attachment_tag
            || !write_id_entry(entries, raster, e, &writer, frame)) {
            return false;
        }
    }
    const raster_buffers_t buffers = raster_carve(raster);
    return !ferror(entries)
           && write_id_pixels(directory, frame, &buffers.picture.attachment[GFX_ATTACHMENT_FURTHER], picture_to_panel,
                              width, height);
}
