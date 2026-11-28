/* stream_provider: indexes recorded render inputs and redraws them from their validated packs. */
#include "stream_provider.h"

#include "collections/mem.h"

#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_bytes.h"
#include "asset/asset_store.h"
#include "core/tune.h"
#include "gfx/gfx.h"
#include "gfx/present/gfx_mode.h"
#include "hash/crc32.h"
#include "render/batch/r3d_batch.h"
#include "render/dynamic/r3d_dynamic_mesh.h"
#include "render/lcol_format.h"
#include "render/record/frame_record_format.h"
#include "render/skin/r3d_skinning.h"
#include "scene/scene_asset.h"
#include "scene/scene_shell.h"

#define STREAM_FILE_CHUNK_BYTES (16U * 1024U)

typedef enum {
    STREAM_SOURCE_POSITIONS,
    STREAM_SOURCE_COLOURS,
    STREAM_SOURCE_TRIANGLES,
    STREAM_SOURCE_UVS,
    STREAM_SOURCE_ARRAYS,
} stream_source_t;

typedef struct {
    size_t begin, end;
    frame_record_frame_t record;
    stream_check_t check;
    frame_record_check_t expected;
    uint32_t crc;
    int width, height;
    bool checked, drawn;
} stream_frame_t;

typedef struct {
    size_t mesh_at, pose_at, built_at;
    r3d_lit_mesh_t mesh;
    mat4f_t model;
    r3d_batch_t batch;
    void* output;
    void* colours;
    size_t output_bytes, colour_bytes;
    void* palette;
    size_t palette_bytes;
    void* source;
    size_t source_bytes;
    frame_record_where_t identity;
    handle_t identity_pack;
    uint32_t* lod;
    size_t lod_words;
    bool dirty, placed;
} stream_mesh_t;

typedef struct {
    handle_t handle;
    uint32_t crc32;
    bool needed;
    char name[FRAME_RECORD_PACK_NAME + 1];
} stream_pack_t;

static struct {
    uint8_t* bytes;
    size_t size, capacity, scanned;
    bool header, checking;
    stream_status_t status;
    stream_frame_t* frames;
    int frame_capacity, next, decoded;
    stream_pack_t* packs;
    size_t pack_count;
    size_t slots[FRAME_RECORD_NO_PACK];
    stream_mesh_t* meshes;
    int mesh_count, instances, batches;
    r3d_instance_t* draws;
    r3d_batch_draw_t* batch_draws;
    render_view_t view;
    frame_record_choice_t choice;
    render_settings_t settings;
    size_t lod_at, lod_bytes;
} stream;

static bool
refuse(const char* format, ...) {
    if (!stream.status.refused) {
        va_list args;
        va_start(args, format);
        vsnprintf(stream.status.why, sizeof stream.status.why, format, args);
        va_end(args);
        stream.status.refused = true;
        fprintf(stderr, "stream: %s\n", stream.status.why);
    }
    return false;
}

static void
free_meshes(void) {
    for (int i = 0; i < stream.mesh_count; i++) {
        free(stream.meshes[i].output);
        free(stream.meshes[i].colours);
        free(stream.meshes[i].lod);
        free(stream.meshes[i].palette);
        free(stream.meshes[i].source);
    }
    free(stream.meshes);
    free(stream.draws);
    free(stream.batch_draws);
    stream.meshes = NULL;
    stream.draws = NULL;
    stream.batch_draws = NULL;
    stream.mesh_count = 0;
}

void
stream_forget(void) {
    free_meshes();
    for (size_t i = 0; i < stream.pack_count; i++) {
        asset_store_release(stream.packs[i].handle);
    }
    free(stream.packs);
    free(stream.bytes);
    free(stream.frames);
    memset(&stream, 0, sizeof stream);
    stream.decoded = -1;
    stream.settings = (render_settings_t)RENDER_SETTINGS_DEFAULT;
}

stream_status_t
stream_status(void) {
    return stream.status;
}

stream_frame_info_t
stream_frame_info(int index) {
    if (index < 0 || index >= stream.status.frames) {
        return (stream_frame_info_t){0};
    }
    const stream_frame_t* f = &stream.frames[index];
    return (stream_frame_info_t){f->record.dt_ms, f->record.flags, f->check};
}

int
stream_next(void) {
    return stream.next;
}

static handle_t
checked_pack(const char* name, uint32_t crc) {
    const handle_t handle = asset_store_load(name);
    const asset_pack_t* pack = asset_store_loaded(handle);
    if (pack == NULL) {
        asset_store_release(handle);
        refuse("missing pack %s", name);
        return HANDLE_NONE;
    }
    const uint32_t actual = asset_pack_checksum(pack);
    if (actual != crc) {
        asset_store_release(handle);
        refuse("pack %s crc32 stream=%08x pack=%08x", name, crc, actual);
        return HANDLE_NONE;
    }
    return handle;
}

static size_t
pack_identity(const char* name, uint32_t crc) {
    for (size_t i = 0; i < stream.pack_count; i++) {
        const stream_pack_t* pack = &stream.packs[i];
        if (pack->crc32 == crc && strcmp(pack->name, name) == 0) {
            return i + 1;
        }
    }
    const handle_t handle = checked_pack(name, crc);
    if (handle_equal(handle, HANDLE_NONE)) {
        return 0;
    }
    asset_store_release(handle);
    void* grown = realloc(stream.packs, (stream.pack_count + 1) * sizeof *stream.packs);
    if (grown == NULL) {
        refuse("no memory for pack table");
        return 0;
    }
    stream.packs = grown;
    stream_pack_t* stored = &stream.packs[stream.pack_count++];
    *stored = (stream_pack_t){.crc32 = crc};
    memcpy(stored->name, name, sizeof stored->name);
    return stream.pack_count;
}

static bool
load_pack(const uint8_t* body, size_t length, bool map_slot) {
    frame_record_pack_t row;
    if (length != sizeof row) {
        return refuse("invalid PACK length");
    }
    memcpy(&row, body, sizeof row);
    char name[FRAME_RECORD_PACK_NAME + 1] = {0};
    memcpy(name, row.name, sizeof row.name);
    if (row.slot == FRAME_RECORD_NO_PACK) {
        return refuse("invalid pack slot for %s", name);
    }
    const size_t identity = pack_identity(name, row.crc32);
    if (identity == 0) {
        return false;
    }
    if (map_slot && stream.slots[row.slot] != identity) {
        stream.slots[row.slot] = identity;
        for (int i = 0; i < stream.mesh_count; i++) {
            stream.meshes[i].dirty = true;
        }
    }
    return true;
}

static void
judge(stream_frame_t* f) {
    if (!f->drawn || !f->checked) {
        return;
    }
    if (f->check == STREAM_CHECK_SAME) {
        stream.status.checks_same--;
    } else if (f->check != STREAM_CHECK_NONE) {
        stream.status.checks_differ--;
    }
    f->check = f->width != f->expected.width || f->height != f->expected.height ? STREAM_CHECK_OTHER_SIZE
               : f->crc == f->expected.crc32                                    ? STREAM_CHECK_SAME
                                                                                : STREAM_CHECK_DIFFERS;
    if (f->check == STREAM_CHECK_SAME) {
        stream.status.checks_same++;
    } else {
        stream.status.checks_differ++;
    }
}

static bool
index_check(size_t at, size_t length) {
    frame_record_check_t check;
    if (length != sizeof check) {
        return refuse("invalid CHECK length");
    }
    memcpy(&check, stream.bytes + at, sizeof check);
    int lo = 0, hi = stream.status.frames;
    while (lo < hi) {
        const int mid = lo + (hi - lo) / 2;
        if (stream.frames[mid].record.index < check.index) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (lo < stream.status.frames && stream.frames[lo].record.index == check.index) {
        stream_frame_t* f = &stream.frames[lo];
        f->expected = check;
        f->checked = true;
        judge(f);
    }
    return true;
}

static bool
index_records(size_t begin, size_t end) {
    while (begin < end) {
        frame_record_head_t head;
        if (end - begin < sizeof head) {
            return refuse("truncated record head");
        }
        memcpy(&head, stream.bytes + begin, sizeof head);
        begin += sizeof head;
        if (head.length > end - begin) {
            return refuse("record leaves its frame");
        }
        if (head.type == FRAME_RECORD_PACK && !load_pack(stream.bytes + begin, head.length, false)) {
            return false;
        }
        if (head.type == FRAME_RECORD_CHECK && !index_check(begin, head.length)) {
            return false;
        }
        begin += head.length;
    }
    return true;
}

/* A stream opens with a HEADER of its fixed length; anything else is refused
 * from its head alone, before waiting on the length it claims. */
static bool
first_head(frame_record_head_t head) {
    return (head.type == FRAME_RECORD_HEADER && head.length == sizeof(frame_record_header_t))
           || refuse("HEADER must be first");
}

static bool
index_header(size_t at) {
    frame_record_header_t header;
    memcpy(&header, stream.bytes + at, sizeof header);
    if (header.version != FRAME_RECORD_VERSION) {
        return refuse("frame record version %u, expected %u", header.version, FRAME_RECORD_VERSION);
    }
    memcpy(stream.status.build_id, header.build_id, sizeof header.build_id);
    stream.header = true;
    stream.decoded = -1;
    return true;
}

static bool
index_frame(frame_record_frame_t row, size_t begin, size_t end) {
    if (stream.status.frames == INT_MAX) {
        return refuse("too many frames");
    }
    if (stream.status.frames == stream.frame_capacity) {
        const int capacity = stream.frame_capacity < INT_MAX / 2 ? (stream.frame_capacity + 1) * 2 : INT_MAX;
        void* grown = realloc(stream.frames, (size_t)capacity * sizeof *stream.frames);
        if (grown == NULL) {
            return refuse("no memory for frame index");
        }
        stream.frames = grown;
        stream.frame_capacity = capacity;
    }
    if (stream.status.frames > 0 && stream.frames[stream.status.frames - 1].record.index >= row.index) {
        return refuse("frame indices must increase");
    }
    stream.frames[stream.status.frames++] = (stream_frame_t){.begin = begin, .end = end, .record = row};
    stream.status.dropped += (row.flags & FRAME_RECORD_AFTER_DROP) != 0;
    return index_records(begin, end);
}

static bool
append_bytes(const uint8_t* bytes, size_t count) {
    if (count > SIZE_MAX - stream.size || (count > 0 && bytes == NULL)) {
        return refuse("invalid stream size");
    }
    const size_t needed = stream.size + count;
    if (needed > stream.capacity) {
        const size_t capacity = needed <= SIZE_MAX / 2 ? needed * 2 : needed;
        void* grown = realloc(stream.bytes, capacity);
        if (grown == NULL) {
            return refuse("no memory for stream bytes");
        }
        stream.bytes = grown;
        stream.capacity = capacity;
    }
    if (count > 0) {
        memcpy(stream.bytes + stream.size, bytes, count);
    }
    stream.size = needed;
    return true;
}

static bool
scan_record(frame_record_head_t head, size_t body, size_t* out_end) {
    size_t end = body + head.length;
    if (!stream.header) {
        if (!index_header(body)) {
            return false;
        }
    } else if (head.type == FRAME_RECORD_FRAME) {
        frame_record_frame_t row;
        if (head.length != sizeof row) {
            return refuse("invalid FRAME length");
        }
        memcpy(&row, stream.bytes + body, sizeof row);
        if (row.bytes > stream.size - end) {
            return false;
        }
        if (!index_frame(row, end, end + row.bytes)) {
            return false;
        }
        end += row.bytes;
    } else if (!index_records(stream.scanned, end)) {
        return false;
    }
    *out_end = end;
    return true;
}

bool
stream_feed(const uint8_t* bytes, size_t count) {
    if (stream.status.refused || !append_bytes(bytes, count)) {
        return false;
    }
    while (stream.size - stream.scanned >= sizeof(frame_record_head_t)) {
        frame_record_head_t head;
        memcpy(&head, stream.bytes + stream.scanned, sizeof head);
        const size_t body = stream.scanned + sizeof head;
        if ((!stream.header && !first_head(head)) || head.length > stream.size - body
            || !scan_record(head, body, &stream.scanned)) {
            break;
        }
    }
    return !stream.status.refused;
}

static const asset_pack_t*
where_pack(frame_record_where_t where) {
    if (where.pack == FRAME_RECORD_NO_PACK || stream.slots[where.pack] == 0) {
        return NULL;
    }
    return asset_store_loaded(stream.packs[stream.slots[where.pack] - 1].handle);
}

static bool
where_entry(frame_record_where_t where, asset_entry_t* entry, size_t bytes) {
    if (asset_pack_entry(where_pack(where), where.entry, entry) != ASSET_OK || where.offset > entry->view.size
        || bytes > entry->view.size - where.offset) {
        return refuse("invalid pack place %u:%u+%u", where.pack, where.entry, where.offset);
    }
    return true;
}

static bool
open_batch(stream_mesh_t* m, frame_record_where_t where) {
    asset_entry_t entry;
    scene_asset_t asset;
    if (!where_entry(where, &entry, sizeof(r3d_copy_place_t)) || scene_asset_open(entry.view, &asset) != ASSET_OK) {
        return refuse("invalid batch scene entry");
    }
    const void* address = entry.view.data + where.offset;
    for (int i = 0; i < asset.batch_count; i++) {
        const scene_asset_batch_t* row = &asset.batches[i];
        if (asset.places + row->first_copy == address) {
            m->batch = scene_asset_batch(&asset, i, &m->mesh, asset.chunks);
            return true;
        }
    }
    return refuse("batch place does not name a batch row");
}

static bool
reserve(void** block, size_t* capacity, size_t bytes) {
    if (*capacity >= bytes && *block != NULL) {
        return true;
    }
    /* Doubled at least: a mesh rebuilt each frame (water) grows a little at
     * a time, and a realloc per frame is what the frame watch reports. */
    const size_t want = bytes > *capacity * 2 ? bytes : *capacity * 2;
    void* grown = realloc(*block, want > 0 ? want : 1);
    if (grown == NULL) {
        return refuse("no memory for replay mesh");
    }
    *block = grown;
    *capacity = want;
    return true;
}

static bool
pose_mesh(stream_mesh_t* m) {
    if (m->pose_at == 0) {
        return refuse("skinned mesh has no POSE");
    }
    frame_record_pose_t pose;
    memcpy(&pose, stream.bytes + m->pose_at, sizeof pose);
    asset_entry_t entry;
    r3d_skin_t skin;
    if (!where_entry(pose.skin, &entry, 0) || r3d_skin_open(where_pack(pose.skin), entry.name, &skin) != ASSET_OK
        || skin.joint_count != pose.joints || skin.vertex_count != (uint32_t)m->mesh.vertex_count
        || (pose.lit != 0 && pose.lit != m->mesh.vertex_count)) {
        return refuse("POSE does not match bind mesh");
    }
    const size_t palette_bytes = (size_t)pose.joints * sizeof(mat4f_t);
    const size_t output_bytes = r3d_skin_output_bytes(&m->mesh);
    if (!reserve(&m->palette, &m->palette_bytes, palette_bytes)
        || !reserve(&m->output, &m->output_bytes, output_bytes)) {
        return false;
    }
    mat4f_t* palette = m->palette;
    r3d_skin_output_t output;
    if (palette == NULL || m->output == NULL || !r3d_skin_output_init(&m->mesh, m->output, output_bytes, &output)) {
        return refuse("invalid skin output");
    }
    memcpy(palette, stream.bytes + m->pose_at + sizeof pose, palette_bytes);
    r3d_skin_clusters(&skin, &m->mesh, palette, 0, m->mesh.cluster_count, &output);
    r3d_skin_refit_nodes(&output);
    m->mesh = output.mesh;
    if (pose.lit > 0) {
        const size_t bytes = (size_t)pose.lit * sizeof m->mesh.colors[0];
        if (!reserve(&m->colours, &m->colour_bytes, bytes)) {
            return false;
        }
        memcpy(m->colours, stream.bytes + m->pose_at + sizeof pose + palette_bytes, bytes);
        m->mesh.colors = m->colours;
    }
    return true;
}

static bool
build_mesh(stream_mesh_t* m) {
    if (m->built_at == 0) {
        return refuse("built mesh has no DYNMESH");
    }
    frame_record_dynmesh_t row;
    memcpy(&row, stream.bytes + m->built_at, sizeof row);
    r3d_dynamic_capacity_t capacity = r3d_dynamic_capacity_for((int)row.vertices, (int)row.triangles);
    capacity.textured = row.textured != 0;
    const size_t bytes = r3d_dynamic_mesh_bytes(capacity);
    if (!reserve(&m->output, &m->output_bytes, bytes)) {
        return false;
    }
    r3d_dynamic_mesh_t built;
    if (m->output == NULL || !r3d_dynamic_mesh_init(&built, m->output, bytes, capacity, row.position_scale)) {
        return refuse("invalid dynamic capacity or no memory");
    }
    const size_t positions = (size_t)row.vertices * sizeof(vec3f_t);
    const size_t colours = (size_t)row.vertices * sizeof(uint8_t[3]);
    const size_t triangles = (size_t)row.triangles * sizeof(uint16_t[3]);
    const size_t uvs = row.textured ? (size_t)row.vertices * sizeof(int16_t[2]) : 0;
    const size_t alignment = _Alignof(vec3f_t);
    const size_t sizes[] = {positions, colours, triangles, uvs};
    size_t offsets[STREAM_SOURCE_ARRAYS];
    size_t source_bytes = 0;
    for (size_t i = 0; i < STREAM_SOURCE_ARRAYS; i++) {
        offsets[i] = mem_carve(&source_bytes, sizes[i], alignment);
    }
    if (!reserve(&m->source, &m->source_bytes, source_bytes)) {
        return false;
    }
    void* p = (uint8_t*)m->source + offsets[STREAM_SOURCE_POSITIONS];
    void* c = (uint8_t*)m->source + offsets[STREAM_SOURCE_COLOURS];
    void* t = (uint8_t*)m->source + offsets[STREAM_SOURCE_TRIANGLES];
    void* u = (uint8_t*)m->source + offsets[STREAM_SOURCE_UVS];
    const uint8_t* source = stream.bytes + m->built_at + sizeof row;
    memcpy(p, source, positions);
    memcpy(c, source + positions, colours);
    memcpy(t, source + positions + colours, triangles);
    memcpy(u, source + positions + colours + triangles, uvs);
    const r3d_mesh_source_t src = {.positions = p,
                                   .colors = c,
                                   .triangles = t,
                                   .vertex_count = (int)row.vertices,
                                   .triangle_count = (int)row.triangles,
                                   .double_sided = row.double_sided != 0,
                                   .uvs = row.textured ? u : NULL};
    const bool fitted = r3d_dynamic_mesh_build(&built, &src);
    m->mesh = built.mesh;
    return fitted == (row.fitted != 0) || refuse("dynamic build fit differs");
}

/* An LCOL set holds its back colours right after its front ones, so a
 * recorded set's backs follow the front colours the record points at. */
static bool
set_backs(stream_mesh_t* m, const asset_entry_t* colours, const frame_record_mesh_t* row, size_t front) {
    const uint32_t backs = asset_read_u32(colours->view.data + LCOL_AT_BACK_COUNT);
    const size_t bytes = (size_t)backs * (row->faces != 0 ? LCOL_RGB565_BYTES : LCOL_RGB888_BYTES);
    if (bytes > colours->view.size - row->colours.offset - front) {
        return refuse("back colours past their entry at %u:%u", row->colours.pack, row->colours.entry);
    }
    const void* back = backs == 0 ? NULL : colours->view.data + row->colours.offset + front;
    m->mesh.back_colors = row->faces != 0 ? NULL : back;
    m->mesh.back_face_colors = row->faces != 0 ? back : NULL;
    m->mesh.back_count = (int)backs;
    return true;
}

static bool
open_mesh(stream_mesh_t* m, const frame_record_mesh_t* row) {
    asset_entry_t entry;
    if (!where_entry(row->positions, &entry, 0)
        || r3d_lit_mesh_open(where_pack(row->positions), entry.name, &m->mesh) != ASSET_OK) {
        return refuse("cannot open mesh at %u:%u", row->positions.pack, row->positions.entry);
    }
    if (row->colours.pack != FRAME_RECORD_NO_PACK) {
        const size_t bytes = row->faces != 0 ? (size_t)m->mesh.triangle_count * sizeof m->mesh.face_colors[0]
                                             : (size_t)m->mesh.vertex_count * sizeof m->mesh.colors[0];
        asset_entry_t colours;
        if (!where_entry(row->colours, &colours, bytes)) {
            return false;
        }
        const uint8_t* at = colours.view.data + row->colours.offset;
        m->mesh.colors = row->faces != 0 ? NULL : (const uint8_t (*)[3])at;
        m->mesh.face_colors = row->faces != 0 ? (const uint16_t*)(const void*)at : NULL;
        if (colours.type == LCOL_ASSET && !set_backs(m, &colours, row, bytes)) {
            return false;
        }
    }
    if (row->kind == FRAME_RECORD_SKINNED && !pose_mesh(m)) {
        return false;
    }
    if (row->kind == FRAME_RECORD_BATCH && !open_batch(m, row->batch)) {
        return false;
    }
    if (row->kind > FRAME_RECORD_BUILT) {
        return refuse("unknown mesh kind %u", row->kind);
    }
    return true;
}

static bool
mesh_lod(stream_mesh_t* m, const frame_record_mesh_t* row) {
    const size_t words = R3D_LOD_STATE_WORDS(m->mesh.lod_group_count);
    const size_t total = words * (m->batch.copy_count > 0 ? m->batch.copy_count : 1);
    if (total != m->lod_words) {
        free(m->lod);
        m->lod = total > 0 ? calloc(total, sizeof *m->lod) : NULL;
        m->lod_words = total;
        if (total > 0 && m->lod == NULL) {
            return refuse("no memory for LOD state");
        }
    }
    const handle_t identity_pack = row->positions.pack == FRAME_RECORD_NO_PACK || stream.slots[row->positions.pack] == 0
                                       ? HANDLE_NONE
                                       : stream.packs[stream.slots[row->positions.pack] - 1].handle;
    if (memcmp(&m->identity, &row->positions, sizeof m->identity) != 0
        || !handle_equal(m->identity_pack, identity_pack)) {
        if (m->lod_words > 0) {
            memset(m->lod, 0, m->lod_words * sizeof *m->lod);
        }
        m->identity = row->positions;
        m->identity_pack = identity_pack;
    }
    m->batch.lod_coarse = m->lod;
    m->batch.lod_words = (uint16_t)words;
    return true;
}

static bool
resolve_mesh(stream_mesh_t* m) {
    if (m->mesh_at == 0) {
        return refuse("missing MESH");
    }
    frame_record_mesh_t row;
    memcpy(&row, stream.bytes + m->mesh_at, sizeof row);
    m->batch = (r3d_batch_t){0};
    memcpy(&m->model, row.model, sizeof m->model);
    m->placed = row.placed != 0;
    if (!(row.kind == FRAME_RECORD_BUILT ? build_mesh(m) : open_mesh(m, &row)) || !mesh_lod(m, &row)) {
        return false;
    }
    m->dirty = false;
    return true;
}

static bool
mesh_slots(int count) {
    if (count <= stream.mesh_count) {
        return true;
    }
    stream_mesh_t* meshes = realloc(stream.meshes, (size_t)count * sizeof *meshes);
    if (meshes == NULL) {
        return refuse("no memory for mesh state");
    }
    stream.meshes = meshes;
    memset(meshes + stream.mesh_count, 0, (size_t)(count - stream.mesh_count) * sizeof *meshes);
    stream.mesh_count = count;
    free(stream.draws);
    free(stream.batch_draws);
    stream.draws = calloc((size_t)count, sizeof *stream.draws);
    stream.batch_draws = calloc((size_t)count, sizeof *stream.batch_draws);
    return (stream.draws != NULL && stream.batch_draws != NULL) || refuse("no memory for draw lists");
}

static bool
read_view(const uint8_t* body, size_t length) {
    if (length != sizeof stream.view) {
        return refuse("invalid VIEW length");
    }
    memcpy(&stream.view, body, length);
    const viewport_t v = stream.view.viewport;
    if (v.width <= 0 || v.width > GFX_WIDTH || v.height <= 0 || v.height > GFX_HEIGHT) {
        return refuse("VIEW size outside compose buffers");
    }
    return (stream.view.present >= CAMERA_PRESENT_NEAREST && stream.view.present <= CAMERA_PRESENT_DOUBLE)
           || refuse("invalid VIEW presentation");
}

static bool
read_choice(const uint8_t* body, size_t length) {
    if (length != sizeof stream.choice) {
        return refuse("invalid CHOICE length");
    }
    memcpy(&stream.choice, body, length);
    return (stream.choice.width > 0 && stream.choice.width <= GFX_WIDTH && stream.choice.height > 0
            && stream.choice.height <= GFX_HEIGHT)
           || refuse("CHOICE size outside compose buffers");
}

static bool
read_tune(const uint8_t* body, size_t length) {
    frame_record_tune_t tune;
    if (length != sizeof tune) {
        return refuse("invalid TUNE length");
    }
    memcpy(&tune, body, sizeof tune);
    char name[FRAME_RECORD_TUNE_NAME + 1] = {0};
    memcpy(name, tune.name, sizeof tune.name);
    (void)tune_set(tune_shared(), name, tune.value);
    return true;
}

static bool
read_dynamic(size_t at, size_t length) {
    frame_record_dynmesh_t row;
    if (length < sizeof row) {
        return refuse("short DYNMESH");
    }
    memcpy(&row, stream.bytes + at, sizeof row);
    if (row.vertices > INT16_MAX || row.triangles > UINT16_MAX) {
        return refuse("dynamic source counts out of range");
    }
    const size_t vertices = (size_t)row.vertices * (sizeof(vec3f_t) + sizeof(uint8_t[3]));
    const size_t triangles = (size_t)row.triangles * sizeof(uint16_t[3]);
    const size_t uvs = row.textured ? (size_t)row.vertices * sizeof(int16_t[2]) : 0;
    if (length != sizeof row + vertices + triangles + uvs) {
        return refuse("invalid DYNMESH length");
    }
    const uint8_t* indices = stream.bytes + at + sizeof row + vertices;
    for (size_t i = 0; i < triangles; i += sizeof(uint16_t)) {
        uint16_t index;
        memcpy(&index, indices + i, sizeof index);
        if (index >= row.vertices) {
            return refuse("dynamic triangle index outside vertices");
        }
    }
    return true;
}

static bool
read_mesh(uint16_t type, size_t at, size_t length, int count) {
    const uint8_t* body = stream.bytes + at;
    uint16_t index;
    if (length < sizeof index) {
        return refuse("short mesh record");
    }
    memcpy(&index, body, sizeof index);
    if (index >= count) {
        return refuse("mesh index %u outside frame", index);
    }
    stream_mesh_t* m = &stream.meshes[index];
    size_t expected = length;
    switch (type) {
        case FRAME_RECORD_MESH:
            expected = sizeof(frame_record_mesh_t);
            m->mesh_at = at;
            break;
        case FRAME_RECORD_POSE: {
            frame_record_pose_t pose;
            if (length < sizeof pose) {
                return refuse("short POSE");
            }
            memcpy(&pose, body, sizeof pose);
            expected = sizeof pose + (size_t)pose.joints * sizeof(mat4f_t) + (size_t)pose.lit * sizeof(uint8_t[3]);
            m->pose_at = at;
            break;
        }
        case FRAME_RECORD_DYNMESH:
            if (!read_dynamic(at, length)) {
                return false;
            }
            m->built_at = at;
            break;
    }
    m->dirty = true;
    return length == expected || refuse("invalid mesh record length");
}

static bool
decode_record(uint16_t type, size_t at, size_t length, int count) {
    const uint8_t* body = stream.bytes + at;
    switch (type) {
        case FRAME_RECORD_LODSTATE:
            if (length < sizeof(frame_record_lodstate_t)) {
                return refuse("short LODSTATE");
            }
            stream.lod_at = at;
            stream.lod_bytes = length;
            return true;
        case FRAME_RECORD_PACK: return load_pack(body, length, true);
        case FRAME_RECORD_VIEW: return read_view(body, length);
        case FRAME_RECORD_CHOICE: return read_choice(body, length);
        case FRAME_RECORD_TUNE: return read_tune(body, length);
        case FRAME_RECORD_MESH:
        case FRAME_RECORD_POSE:
        case FRAME_RECORD_DYNMESH: return read_mesh(type, at, length, count);
        case FRAME_RECORD_SETTINGS:
            if (length != sizeof stream.settings) {
                return refuse("invalid SETTINGS length");
            }
            memcpy(&stream.settings, body, length);
            return true;
        default: return true;
    }
}

static bool
need_pack(frame_record_where_t where) {
    if (where.pack == FRAME_RECORD_NO_PACK) {
        return true;
    }
    const size_t identity = stream.slots[where.pack];
    if (identity == 0) {
        return refuse("unnamed pack slot %u", where.pack);
    }
    stream.packs[identity - 1].needed = true;
    return true;
}

static bool
mesh_packs(const stream_mesh_t* mesh) {
    if (mesh->mesh_at == 0) {
        return refuse("missing MESH");
    }
    frame_record_mesh_t row;
    memcpy(&row, stream.bytes + mesh->mesh_at, sizeof row);
    if (row.kind == FRAME_RECORD_BUILT) {
        return true;
    }
    if (!need_pack(row.positions) || !need_pack(row.colours)) {
        return false;
    }
    if (row.kind == FRAME_RECORD_BATCH && !need_pack(row.batch)) {
        return false;
    }
    if (row.kind == FRAME_RECORD_SKINNED && mesh->pose_at != 0) {
        frame_record_pose_t pose;
        memcpy(&pose, stream.bytes + mesh->pose_at, sizeof pose);
        return need_pack(pose.skin);
    }
    return true;
}

static bool
mount_packs(int count) {
    for (size_t i = 0; i < stream.pack_count; i++) {
        stream.packs[i].needed = false;
    }
    for (int i = 0; i < count; i++) {
        if (!mesh_packs(&stream.meshes[i])) {
            return false;
        }
    }
    for (size_t i = 0; i < stream.pack_count; i++) {
        stream_pack_t* pack = &stream.packs[i];
        if (!pack->needed) {
            asset_store_release(pack->handle);
            pack->handle = HANDLE_NONE;
        }
    }
    for (size_t i = 0; i < stream.pack_count; i++) {
        stream_pack_t* pack = &stream.packs[i];
        if (pack->needed && handle_equal(pack->handle, HANDLE_NONE)) {
            pack->handle = checked_pack(pack->name, pack->crc32);
            if (handle_equal(pack->handle, HANDLE_NONE)) {
                return false;
            }
        }
    }
    return true;
}

static void
begin_keyframe(void) {
    for (const tune_entry_t* t = tune_shared()->first; t != NULL; t = t->next) {
        (void)tune_reset(tune_shared(), t->name);
    }
    free_meshes();
    memset(stream.slots, 0, sizeof stream.slots);
    stream.view = (render_view_t){0};
    stream.choice = (frame_record_choice_t){0};
    stream.settings = (render_settings_t)RENDER_SETTINGS_DEFAULT;
}

static bool
restore_lod(int count) {
    if (stream.lod_at == 0) {
        return true;
    }
    frame_record_lodstate_t state;
    const uint8_t* body = stream.bytes + stream.lod_at;
    memcpy(&state, body, sizeof state);
    render_context_t* context = render_context_main();
    context->raster.pick_round = state.pick_round;
    memcpy(&context->steer, &state.steer, sizeof context->steer);
    size_t at = sizeof state;
    for (int i = 0; i < count; i++) {
        uint32_t words;
        if (stream.lod_bytes - at < sizeof words) {
            return refuse("short LODSTATE mesh");
        }
        memcpy(&words, body + at, sizeof words);
        at += sizeof words;
        stream_mesh_t* mesh = &stream.meshes[i];
        if (words != mesh->lod_words || words > (stream.lod_bytes - at) / sizeof(uint32_t)) {
            return refuse("LODSTATE mesh size differs");
        }
        if (words > 0) {
            memcpy(mesh->lod, body + at, (size_t)words * sizeof(uint32_t));
        }
        at += (size_t)words * sizeof(uint32_t);
    }
    return at == stream.lod_bytes || refuse("invalid LODSTATE length");
}

static bool
frame_size_ok(void) {
    if (stream.choice.width == 0 || stream.choice.height == 0 || stream.view.viewport.width <= 0
        || stream.view.viewport.height <= 0) {
        return refuse("missing VIEW or CHOICE size");
    }
    return (stream.choice.width <= stream.view.viewport.width && stream.choice.height <= stream.view.viewport.height)
           || refuse("CHOICE size exceeds VIEW");
}

static bool
resolve_frame(int count, bool generated) {
    if (!generated && count == 0) {
        return refuse("empty drawn frame");
    }
    if (!mount_packs(count)) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (stream.meshes[i].dirty && !resolve_mesh(&stream.meshes[i])) {
            return false;
        }
    }
    return restore_lod(count);
}

static bool
decode(int index) {
    const stream_frame_t* f = &stream.frames[index];
    if (f->record.flags & FRAME_RECORD_KEYFRAME) {
        begin_keyframe();
    }
    if (!(f->record.flags & FRAME_RECORD_GENERATED)) {
        stream.instances = f->record.instances;
        stream.batches = f->record.batches;
    }
    const int count = stream.instances + stream.batches;
    if (!mesh_slots(count)) {
        return false;
    }
    stream.lod_at = 0;
    for (size_t at = f->begin; at < f->end;) {
        frame_record_head_t head;
        memcpy(&head, stream.bytes + at, sizeof head);
        at += sizeof head;
        if (!decode_record(head.type, at, head.length, count)) {
            return false;
        }
        at += head.length;
    }
    if (!frame_size_ok() || !resolve_frame(count, (f->record.flags & FRAME_RECORD_GENERATED) != 0)) {
        return false;
    }
    stream.decoded = index;
    return true;
}

static void draw(const provider_frame_t* frame);

bool
stream_seek(int index) {
    if (stream.status.refused || index < 0 || index >= stream.status.frames) {
        return false;
    }
    int key = index;
    while (key > 0 && !(stream.frames[key].record.flags & FRAME_RECORD_KEYFRAME)) {
        key--;
    }
    if (!(stream.frames[key].record.flags & (FRAME_RECORD_KEYFRAME | FRAME_RECORD_GENERATED))) {
        return refuse("no keyframe before frame %d", index);
    }
    free_meshes();
    render_context_forget(render_context_main());
    stream.instances = stream.batches = 0;
    stream.settings = (render_settings_t)RENDER_SETTINGS_DEFAULT;
    memset(stream.slots, 0, sizeof stream.slots);
    stream.decoded = -1;
    stream.next = key;
    if (!decode(key)) {
        return false;
    }
    while (stream.next < index) {
        draw(NULL);
        if (stream.status.refused) {
            return false;
        }
    }
    if (stream.decoded != index && !decode(index)) {
        return false;
    }
    stream.next = index;
    return true;
}

static bool
read_file(const char* path) {
    stream_forget();
    FILE* file = fopen(path, "rb");
    if (file == NULL) {
        return refuse("cannot read stream %s", path);
    }
    uint8_t bytes[STREAM_FILE_CHUNK_BYTES];
    bool ok = true;
    size_t count;
    while (ok && (count = fread(bytes, 1, sizeof bytes, file)) > 0) {
        ok = stream_feed(bytes, count);
    }
    ok = ok && !ferror(file);
    fclose(file);
    if (!ok || !stream.header || stream.scanned != stream.size) {
        return refuse("incomplete stream file %s", path);
    }
    return true;
}

static bool
options(int argc, char** argv) {
    const char* path = NULL;
    bool checking = false;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--check") == 0) {
            checking = true;
        } else if (strcmp(argv[i], "--stream") == 0 && i + 1 < argc) {
            path = argv[++i];
        } else {
            return refuse("unknown option %s", argv[i]);
        }
    }
    if (path != NULL && !read_file(path)) {
        return false;
    }
    stream.checking = checking;
    return !stream.status.refused;
}

static int
frame_count(void) {
    return stream.status.frames;
}

static bool
setup(int quarter) {
    (void)quarter;
    stream.next = 0;
    stream.decoded = -1;
    return !stream.status.refused;
}

static void
draw_lists(render_context_t* context) {
    for (int i = 0; i < stream.instances + stream.batches; i++) {
        stream_mesh_t* m = &stream.meshes[i];
        const r3d_instance_t instance = {&m->mesh, m->placed ? &m->model : NULL, NULL, m->lod};
        if (i < stream.instances) {
            stream.draws[i] = instance;
        } else {
            stream.batch_draws[i - stream.instances] = (r3d_batch_draw_t){instance, &m->batch, 0};
        }
    }
    render_context_set_batches(context, stream.batch_draws, stream.batches);
}

static void
draw(const provider_frame_t* frame) {
    (void)frame;
    const int index = stream.next;
    if (stream.status.refused || index >= stream.status.frames) {
        return;
    }
    if (stream.decoded != index && !(index > 0 && stream.decoded == index - 1 ? decode(index) : stream_seek(index))) {
        render_host_fail();
        return;
    }
    render_context_t* context = render_context_main();
    render_context_set_settings(context, &stream.settings);
    const resolution_step_t size = {stream.choice.width, stream.choice.height};
    render_context_set_raster_size(context, size);
    render_context_set_changed(context, NULL, -1);
    draw_lists(context);
    const bool generated = (stream.frames[index].record.flags & FRAME_RECORD_GENERATED) != 0;
    if (generated && !context->kept) {
        stream.next++;
        return;
    }
    const bool rendered =
        generated ? render_context_generate(context, &stream.view)
                  : render_context_draw(context, stream.draws, stream.instances, &stream.view, stream.choice.clear);
    if (!rendered) {
        refuse("render failed at frame %d", index);
        render_host_fail();
        return;
    }
    const bool half = scene_shell_compose_drawn();
    stream_frame_t* f = &stream.frames[index];
    f->width = half ? GFX_WIDTH / 2 : GFX_WIDTH;
    f->height = half ? GFX_HEIGHT / 2 : GFX_HEIGHT;
    const uint16_t* pixels = half ? gfx_half_picture() : gfx_framebuffer();
    f->crc = crc32_update(0, pixels, (size_t)f->width * (size_t)f->height * sizeof *pixels);
    f->drawn = true;
    judge(f);
    stream.next++;
}

static void
teardown(void) {
    for (int i = 0; i < stream.status.frames; i++) {
        const stream_frame_t* f = &stream.frames[i];
        if (f->check == STREAM_CHECK_NONE) {
            continue;
        }
        const char* verdict = f->check == STREAM_CHECK_SAME         ? "SAME"
                              : f->check == STREAM_CHECK_OTHER_SIZE ? "OTHER_SIZE"
                                                                    : "DIFFERS";
        fprintf(stderr, "CHECK %u %s recorded=%08x replay=%08x size=%dx%d recorded_size=%ux%u\n", f->record.index,
                verdict, f->expected.crc32, f->crc, f->width, f->height, f->expected.width, f->expected.height);
    }
    if (stream.status.refused || (stream.checking && stream.status.checks_differ > 0)) {
        render_host_fail();
    }
    free_meshes();
    for (size_t i = 0; i < stream.pack_count; i++) {
        asset_store_release(stream.packs[i].handle);
    }
    free(stream.packs);
    stream.packs = NULL;
    stream.pack_count = 0;
    memset(stream.slots, 0, sizeof stream.slots);
}

const frame_provider_t stream_provider = {
    .name = "stream_render",
    .quarter = 1,
    .options = options,
    .frame_count = frame_count,
    .setup = setup,
    .draw = draw,
    .teardown = teardown,
    .render_context = render_context_main,
};
