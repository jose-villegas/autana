/* The renderer's halves of render_host.c: --set render.FIELD=value reaches
 * the main render context as an app's pack sets it, and --poses reads the
 * active scene camera. */
#include <limits.h>
#include <string.h>
#include "render_host.h"

#include "scene/camera/camera.h"
#include "scene/transform/transform.h"

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

#include <stdlib.h>
#include "asset/asset_store.h"
#include "render/batch/r3d_batch.h"
#include "render/r3d_span.h"
#include "render/raster.h"
#include "render/raster_internal.h"

#define ID_FILE_SUFFIX "/0000.ids"
#define ID_WORD_BYTES  sizeof(uint32_t)
#define ID_AXES        3

static bool id_entries_written;

static bool
write_id_pixels(const char* directory, int frame, const image_t* image) {
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
    for (int y = 0; y < image->height; y++) {
        const uint8_t* row = (const uint8_t*)image->pixels + y * image->pitch;
        for (int x = 0; x < image->width; x++) {
            const uint32_t value = image->format == IMAGE_U32 ? ((const uint32_t*)row)[x] : ((const uint16_t*)row)[x];
            uint8_t bytes[ID_WORD_BYTES];
            for (size_t b = 0; b < ID_WORD_BYTES; b++) {
                bytes[b] = (uint8_t)(value >> (b * CHAR_BIT));
            }
            ok &= fwrite(bytes, sizeof bytes, 1, file) == 1;
        }
    }
    return fclose(file) == 0 && ok;
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
    (void)fprintf(file,
                  "%s{\"frame\":%d,\"entry\":\"%s\",\"first_id\":%u,\"id_count\":%d,"
                  "\"triangle_count\":%d,\"step\":\"%s\",\"matrix\":[",
                  id_entries_written ? "," : "", frame, asset.name, (unsigned)writer->value, count,
                  instance->mesh->triangle_count, step);
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
render_host_write_ids(const char* directory, FILE* entries, int frame) {
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
    if (frame == 0) {
        const lens_t* lens = camera_lens(camera_active());
        if (lens == NULL) {
            return false;
        }
        id_entries_written = false;
        (void)fprintf(entries, "{\"width\":%d,\"height\":%d,\"lens\":%.9g,\"near\":%.9g,\"entries\":[", raster->width,
                      raster->height, (double)lens->half_fov_short_tan, (double)lens->near_z);
    }
    for (int e = 0; e < raster_drawn_count(raster); e++) {
        r3d_span_writer_t writer = {0};
        if (!attachment->writer(attachment, e, &writer) || writer.span != raster_attachment_tag
            || !write_id_entry(entries, raster, e, &writer, frame)) {
            return false;
        }
    }
    const raster_buffers_t buffers = raster_carve(raster);
    return !ferror(entries) && write_id_pixels(directory, frame, &buffers.picture.attachment[GFX_ATTACHMENT_FURTHER]);
}
