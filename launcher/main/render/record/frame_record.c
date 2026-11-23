/* frame_record: see frame_record.h; the bytes are frame_record_format.h's. */
#include "render/record/frame_record.h"

#include <string.h>

#include "asset/asset_store.h"
#include "core/memory.h"
#include "core/tune.h"
#include "esp_log.h"
#include "gfx/gfx.h"
#include "hash/crc32.h"
#include "profile/frame_cost.h"
#include "render/record/frame_record_format.h"

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST

#ifdef ESP_PLATFORM
#include "services/build_id.h"
#endif

/* Drawn meshes whose last recorded place a delta frame compares against. */
#define RECORDED_SLOTS_MAX 512
/* A frame's bytes before the buffer grows. */
#define FRAME_BYTES_FIRST  (32U * 1024U)

/* Keyed by positions: a scene draws its own copy of the mesh struct. The
 * bind is kept by value, and a drawn mesh matches a note only with the same
 * vertex count. A full table reuses its oldest note, and initialising a mesh
 * in a block forgets the notes inside it (frame_record_forget). */
typedef struct {
    const void* key;
    int vertex_count;
    uint32_t noted_at;
} note_head_t;

typedef struct {
    note_head_t head; /* the posed positions */
    const void* bind_positions;
    const void* skin_vertices; /* the skin asset's, in a pack */
    const mat4f_t* palette;
    uint16_t joints;
    bool fresh; /* posed since the last recorded frame */
} noted_skin_t;

typedef struct {
    note_head_t head; /* the built positions */
    int position_scale;
    r3d_mesh_source_t source;
    bool fitted;
    bool fresh; /* built since the last recorded frame */
} noted_built_t;

/* The struct and its arrays: an owner may rewrite a mesh struct in place. */
typedef struct {
    const void* mesh;
    const void* positions;
    const void* colours;
    const void* batch;
    mat4f_t model;
    bool placed;
} recorded_slot_t;

typedef struct {
    noted_skin_t skins[FRAME_RECORD_NOTED_SKINS_MAX];
    noted_built_t built[FRAME_RECORD_NOTED_BUILT_MAX];
    uint32_t made;
} notes_t;

/* In external RAM, taken once and kept: on-chip RAM is what a skin's own
 * buffers want. */
static notes_t* notes;

/* A table of notes as the helpers below take it: its first note, stride and length. */
#define NOTE_TABLE(field) notes->field, sizeof notes->field[0], (int)(sizeof notes->field / sizeof notes->field[0])

typedef struct {
    frame_record_out_t out;
    uint32_t keyframe_every;
    uint32_t index;
    bool keyframe_next;
    bool after_drop;
    uint8_t* bytes;
    size_t size, capacity;
    bool overflowed;
    uint8_t* lod_state;
    size_t lod_size, lod_capacity;
    recorded_slot_t* slots;
    uint32_t packs_named; /* bit per asset_store slot, since the last keyframe or mount change */
    uint32_t mounts_seen; /* asset_store_mount_epoch() when the meshes were last all recorded */
    int32_t* tuned;       /* each tunable's value as last recorded, in registry order */
    int tunables;
    bool check_due; /* the last frame was a keyframe the sink took: hash what composes from it */
    bool checked;   /* `check` holds a hash for the next frame's records */
    frame_record_check_t check;
    render_settings_t settings; /* as last recorded */
    frame_record_counts_t counts;
} recording_t;

static recording_t* rec;

/* The note keyed `key` in a table of `count` notes `stride` apart, else an
 * empty one, else the oldest, stamped for `key` and `vertex_count`. */
static void*
note_stamp(void* table, size_t stride, int count, const void* key, int vertex_count) {
    note_head_t* slot = table;
    for (int i = 0; i < count; i++) {
        note_head_t* note = (note_head_t*)((uint8_t*)table + ((size_t)i * stride));
        if (note->key == key) {
            slot = note;
            break;
        }
        if (slot->key != NULL && (note->key == NULL || note->noted_at < slot->noted_at)) {
            slot = note;
        }
    }
    memset(slot, 0, stride);
    *slot = (note_head_t){key, vertex_count, ++notes->made};
    return slot;
}

/* The note a mesh drawn from `key` with `vertex_count` vertices matches, else NULL. */
static void*
note_find(void* table, size_t stride, int count, const void* key, int vertex_count) {
    for (int i = 0; i < count; i++) {
        note_head_t* note = (note_head_t*)((uint8_t*)table + ((size_t)i * stride));
        if (note->key == key && note->vertex_count == vertex_count) {
            return note;
        }
    }
    return NULL;
}

static bool
inside(const void* address, const void* block, size_t bytes) {
    return (const uint8_t*)address >= (const uint8_t*)block && (const uint8_t*)address < (const uint8_t*)block + bytes;
}

static void
note_forget(void* table, size_t stride, int count, const void* block, size_t bytes) {
    for (int i = 0; i < count; i++) {
        note_head_t* note = (note_head_t*)((uint8_t*)table + ((size_t)i * stride));
        if (inside(note->key, block, bytes)) {
            memset(note, 0, stride);
        }
    }
}

bool
frame_record_take_notes(void) {
    if (notes == NULL) {
        notes = memory_alloc(sizeof *notes, MEMORY_PSRAM);
        if (notes != NULL) {
            memset(notes, 0, sizeof *notes);
        }
    }
    return notes != NULL;
}

void
frame_record_note_skin(const r3d_lit_mesh_t* posed, const r3d_lit_mesh_t* bind, const r3d_skin_t* skin,
                       const mat4f_t* palette, int joints) {
    if (notes != NULL) {
        noted_skin_t* note = note_stamp(NOTE_TABLE(skins), posed->positions, bind->vertex_count);
        note->bind_positions = bind->positions;
        note->skin_vertices = skin->vertices;
        note->palette = palette;
        note->joints = (uint16_t)joints;
        note->fresh = true;
    }
}

void
frame_record_note_built(const r3d_lit_mesh_t* mesh, const r3d_mesh_source_t* source, bool fitted) {
    if (notes != NULL) {
        noted_built_t* note = note_stamp(NOTE_TABLE(built), mesh->positions, mesh->vertex_count);
        note->position_scale = mesh->position_scale;
        note->source = *source;
        note->fitted = fitted;
        note->fresh = true;
    }
}

void
frame_record_forget(const void* block, size_t bytes) {
    if (notes != NULL) {
        note_forget(NOTE_TABLE(skins), block, bytes);
        note_forget(NOTE_TABLE(built), block, bytes);
    }
}

#if BUILD_VARIANT_SELFTEST_OR_HOST
void
frame_record_forget_notes(void) {
    if (notes != NULL) {
        memset(notes, 0, sizeof *notes);
    }
}
#endif

static noted_skin_t*
skin_of(const r3d_lit_mesh_t* mesh) {
    return notes != NULL ? note_find(NOTE_TABLE(skins), mesh->positions, mesh->vertex_count) : NULL;
}

static noted_built_t*
built_of(const r3d_lit_mesh_t* mesh) {
    return notes != NULL ? note_find(NOTE_TABLE(built), mesh->positions, mesh->vertex_count) : NULL;
}

static bool
reserve(size_t more) {
    if (rec->size + more <= rec->capacity) {
        return true;
    }
    size_t capacity = rec->capacity * 2;
    while (capacity < rec->size + more) {
        capacity *= 2;
    }
    uint8_t* bigger = memory_alloc(capacity, MEMORY_PSRAM);
    if (bigger == NULL) {
        rec->overflowed = true;
        return false;
    }
    memcpy(bigger, rec->bytes, rec->size);
    memory_free(rec->bytes);
    rec->bytes = bigger;
    rec->capacity = capacity;
    return true;
}

static void
put(const void* bytes, size_t count) {
    if (reserve(count)) {
        memcpy(rec->bytes + rec->size, bytes, count);
        rec->size += count;
    }
}

static void
head(frame_record_type_t type, size_t length) {
    const frame_record_head_t h = {(uint16_t)type, (uint32_t)length};
    put(&h, sizeof h);
}

static void
record(frame_record_type_t type, const void* body, size_t length) {
    head(type, length);
    put(body, length);
}

_Static_assert(ASSET_STORE_MOUNTS_MAX <= 32, "packs_named holds a bit per asset_store slot");

/* Where `address` sits in a pack, naming the pack first if this keyframe
 * interval has not. */
static frame_record_where_t
where(const void* address) {
    asset_location_t at;
    if (address == NULL || !asset_store_locate(address, &at)) {
        return (frame_record_where_t){FRAME_RECORD_NO_PACK, FRAME_RECORD_NO_ENTRY, 0};
    }
    if ((rec->packs_named & (1U << at.slot)) == 0) {
        frame_record_pack_t pack = {.slot = (uint8_t)at.slot, .crc32 = at.pack_crc};
        (void)strncpy(pack.name, at.pack_name, sizeof pack.name - 1);
        record(FRAME_RECORD_PACK, &pack, sizeof pack);
        rec->packs_named |= 1U << at.slot;
    }
    return (frame_record_where_t){(uint8_t)at.slot, at.entry, at.offset};
}

/* A mesh's colours as the pipeline picks them: per triangle when it has
 * those, else per vertex, else none. */
static const void*
colours_of(const r3d_lit_mesh_t* mesh) {
    return mesh->face_colors != NULL ? (const void*)mesh->face_colors : (const void*)mesh->colors;
}

static bool
moved(int index, const r3d_lit_mesh_t* mesh, const void* batch, const mat4f_t* model) {
    if (index >= RECORDED_SLOTS_MAX) {
        return true;
    }
    recorded_slot_t* slot = &rec->slots[index];
    const recorded_slot_t now = {
        mesh, mesh->positions, colours_of(mesh), batch, model != NULL ? *model : mat4f_identity(), model != NULL};
    const bool same = slot->mesh == now.mesh && slot->positions == now.positions && slot->colours == now.colours
                      && slot->batch == now.batch && slot->placed == now.placed
                      && memcmp(&slot->model, &now.model, sizeof now.model) == 0;
    *slot = now;
    return !same;
}

/* A posed mesh's colours are its own when lit as it was posed: no pack holds them. */
static bool
lit_by_pose(const r3d_lit_mesh_t* posed) {
    asset_location_t at;
    return posed->colors != NULL && !asset_store_locate(posed->colors, &at);
}

static void
record_pose(int index, const r3d_lit_mesh_t* posed, noted_skin_t* skin, bool keyframe) {
    if (!keyframe && !skin->fresh) {
        return;
    }
    const bool lit = lit_by_pose(posed);
    const frame_record_pose_t pose = {(uint16_t)index, skin->joints, lit ? (uint16_t)posed->vertex_count : 0U,
                                      where(skin->skin_vertices)};
    const size_t colours = (size_t)pose.lit * sizeof posed->colors[0];
    head(FRAME_RECORD_POSE, sizeof pose + ((size_t)skin->joints * sizeof(mat4f_t)) + colours);
    put(&pose, sizeof pose);
    put(skin->palette, (size_t)skin->joints * sizeof(mat4f_t));
    if (lit) {
        put(posed->colors, colours);
    }
}

static void
record_built(int index, noted_built_t* b, bool keyframe) {
    if (!keyframe && !b->fresh) {
        return;
    }
    const r3d_mesh_source_t* s = &b->source;
    const size_t positions = (size_t)s->vertex_count * sizeof s->positions[0];
    const size_t colours = (size_t)s->vertex_count * sizeof s->colors[0];
    const size_t triangles = (size_t)s->triangle_count * sizeof s->triangles[0];
    const size_t uvs = s->uvs != NULL ? (size_t)s->vertex_count * sizeof s->uvs[0] : 0;
    const frame_record_dynmesh_t mesh = {.index = (uint16_t)index,
                                         .fitted = b->fitted,
                                         .double_sided = s->double_sided,
                                         .position_scale = b->position_scale,
                                         .vertices = (uint32_t)s->vertex_count,
                                         .triangles = (uint32_t)s->triangle_count,
                                         .textured = s->uvs != NULL};
    head(FRAME_RECORD_DYNMESH, sizeof mesh + positions + colours + triangles + uvs);
    put(&mesh, sizeof mesh);
    put(s->positions, positions);
    put(s->colors, colours);
    put(s->triangles, triangles);
    if (uvs > 0) {
        put(s->uvs, uvs);
    }
}

/* After a frame: every instance drawing a fresh mesh has carried it. */
static void
clear_fresh(void) {
    notes_t* table = notes;
    for (int i = 0; table != NULL && i < FRAME_RECORD_NOTED_SKINS_MAX; i++) {
        table->skins[i].fresh = false;
    }
    for (int i = 0; table != NULL && i < FRAME_RECORD_NOTED_BUILT_MAX; i++) {
        table->built[i].fresh = false;
    }
}

/* A MESH record: where instance `index`'s mesh, colours and batch come from, and its place. */
static void
record_mesh_place(int index, const r3d_instance_t* instance, const r3d_batch_t* batch, frame_record_kind_t kind,
                  const void* positions) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    frame_record_mesh_t m = {.index = (uint16_t)index,
                             .kind = (uint8_t)kind,
                             .placed = instance->model != NULL,
                             .faces = mesh->face_colors != NULL};
    m.positions = where(positions);
    m.colours = where(colours_of(mesh));
    m.batch = where(batch != NULL ? (const void*)batch->places : NULL);
    const mat4f_t model = instance->model != NULL ? *instance->model : mat4f_identity();
    memcpy(m.model, &model, sizeof m.model);
    record(FRAME_RECORD_MESH, &m, sizeof m);
}

static frame_record_kind_t
mesh_kind(bool batched, bool skinned, bool built_here) {
    if (batched) {
        return FRAME_RECORD_BATCH;
    }
    if (skinned) {
        return FRAME_RECORD_SKINNED;
    }
    return built_here ? FRAME_RECORD_BUILT : FRAME_RECORD_STATIC;
}

static void
record_mesh(int index, const r3d_instance_t* instance, const r3d_batch_t* batch, bool keyframe) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    noted_skin_t* skin = skin_of(mesh);
    noted_built_t* b = skin == NULL ? built_of(mesh) : NULL;
    if (moved(index, mesh, batch, instance->model) || keyframe) {
        record_mesh_place(index, instance, batch, mesh_kind(batch != NULL, skin != NULL, b != NULL),
                          skin != NULL ? skin->bind_positions : (const void*)mesh->positions);
    }
    if (skin != NULL) {
        record_pose(index, mesh, skin, keyframe);
    } else if (b != NULL) {
        record_built(index, b, keyframe);
    }
}

/* On a keyframe each tunable off its initial value, else each that changed:
 * a reader starts from the same build's initial values. */
static void
record_tunables(bool keyframe) {
#if TUNE_ENABLED
    int i = 0;
    for (const tune_entry_t* e = tune_shared()->first; e != NULL && i < rec->tunables; e = e->next, i++) {
        const int32_t value = *e->value;
        if (keyframe ? value != e->initial : value != rec->tuned[i]) {
            frame_record_tune_t t = {.value = value};
            const size_t length = strlen(e->name);
            memcpy(t.name, e->name, length < sizeof t.name ? length : sizeof t.name);
            record(FRAME_RECORD_TUNE, &t, sizeof t);
        }
        rec->tuned[i] = value;
    }
#else
    (void)keyframe;
#endif
}

static int
count_tunables(void) {
#if TUNE_ENABLED
    return tune_count(tune_shared());
#else
    return 0;
#endif
}

static void
record_header(void) {
    frame_record_header_t h = {FRAME_RECORD_VERSION, GFX_WIDTH, GFX_HEIGHT, (uint16_t)rec->keyframe_every, {0}};
#ifdef ESP_PLATFORM
    (void)strncpy(h.build_id, build_id(), sizeof h.build_id - 1);
#else
    (void)strncpy(h.build_id, "host", sizeof h.build_id - 1);
#endif
    record(FRAME_RECORD_HEADER, &h, sizeof h);
}

bool
frame_record_start(const frame_record_out_t* out, uint32_t keyframe_every) {
    if (rec != NULL) {
        return false;
    }
    rec = memory_alloc(sizeof *rec, MEMORY_PSRAM);
    if (rec == NULL) {
        return false;
    }
    *rec = (recording_t){.out = *out, .keyframe_every = keyframe_every > 0 ? keyframe_every : 1, .keyframe_next = true};
    rec->bytes = memory_alloc(FRAME_BYTES_FIRST, MEMORY_PSRAM);
    rec->slots = memory_alloc(RECORDED_SLOTS_MAX * sizeof *rec->slots, MEMORY_PSRAM);
    rec->tunables = count_tunables();
    rec->tuned = memory_alloc(((size_t)rec->tunables + 1) * sizeof *rec->tuned, MEMORY_PSRAM);
    if (rec->bytes == NULL || rec->slots == NULL || rec->tuned == NULL) {
        frame_record_stop();
        return false;
    }
    rec->capacity = FRAME_BYTES_FIRST;
    rec->mounts_seen = asset_store_mount_epoch();
    record_header();
    if (!rec->out.write(rec->out.sink, rec->bytes, rec->size)) {
        frame_record_stop();
        return false;
    }
    return true;
}

void
frame_record_stop(void) {
    if (rec == NULL) {
        return;
    }
    if (rec->checked) {
        /* The last keyframe's hash has no frame after it to ride in. */
        rec->size = 0;
        record(FRAME_RECORD_CHECK, &rec->check, sizeof rec->check);
        (void)rec->out.write(rec->out.sink, rec->bytes, rec->size);
    }
    memory_free(rec->lod_state);
    memory_free(rec->tuned);
    memory_free(rec->bytes);
    memory_free(rec->slots);
    memory_free(rec);
    rec = NULL;
}

bool
frame_record_active(void) {
    return rec != NULL;
}

frame_record_counts_t
frame_record_counts(void) {
    return rec != NULL ? rec->counts : (frame_record_counts_t){0};
}

/* The frame's own records: FRAME, VIEW, CHOICE, a pending CHECK, the tunables. */
static void
record_frame_head(const frame_record_context_t* context, const raster_t* raster, uint32_t dt_ms, bool keyframe) {
    const bool generated = context->frame->generated;
    const frame_record_frame_t frame = {
        .index = rec->index,
        .dt_ms = dt_ms,
        .flags = (uint8_t)((keyframe ? FRAME_RECORD_KEYFRAME : 0U) | (rec->after_drop ? FRAME_RECORD_AFTER_DROP : 0U)
                           | (generated ? FRAME_RECORD_GENERATED : 0U)),
        .instances = generated ? 0U : (uint16_t)raster->instance_count,
        .batches = generated ? 0U : (uint16_t)raster->batch_count,
    };
    record(FRAME_RECORD_FRAME, &frame, sizeof frame);
    record(FRAME_RECORD_VIEW, context->view, sizeof *context->view);
    const frame_record_choice_t choice = {(int16_t)context->frame->step,   generated,
                                          (uint8_t)context->scale_percent, raster->clear,
                                          (uint16_t)context->frame->width, (uint16_t)context->frame->height};
    record(FRAME_RECORD_CHOICE, &choice, sizeof choice);
    if (rec->checked) {
        record(FRAME_RECORD_CHECK, &rec->check, sizeof rec->check);
    }
    const render_settings_t* settings = context->settings;
    if (keyframe || memcmp(settings, &rec->settings, sizeof *settings) != 0) {
        record(FRAME_RECORD_SETTINGS, settings, sizeof *settings);
        rec->settings = *settings;
    }
    record_tunables(keyframe);
}

static void
record_meshes(const raster_t* raster, bool keyframe) {
    for (int i = 0; i < raster->instance_count; i++) {
        record_mesh(i, &raster->instances[i], NULL, keyframe);
    }
    for (int i = 0; i < raster->batch_count; i++) {
        record_mesh(raster->instance_count + i, &raster->batches[i].instance, raster->batches[i].batch, keyframe);
    }
}

/* After the sink took the frame or not: what the next frame owes. */
static void
settle(bool taken, bool keyframe_due, bool keyframe) {
    if (taken) {
        rec->counts.frames++;
    } else {
        rec->counts.dropped++;
    }
    rec->checked = rec->checked && !taken;
    rec->keyframe_next = !taken || (keyframe_due && !keyframe);
    rec->after_drop = !taken;
    rec->check_due = taken && keyframe;
}

static size_t
snapshot_lod_mesh(uint8_t* out, const r3d_instance_t* instance, const r3d_batch_t* batch) {
    const uint32_t words =
        R3D_LOD_STATE_WORDS(instance->mesh->lod_group_count) * (batch != NULL ? batch->copy_count : 1);
    if (out != NULL) {
        const uint32_t* coarse = batch != NULL ? batch->lod_coarse : instance->lod_coarse;
        memcpy(out, &words, sizeof words);
        if (coarse != NULL) {
            memcpy(out + sizeof words, coarse, (size_t)words * sizeof *coarse);
        } else {
            memset(out + sizeof words, 0, (size_t)words * sizeof(uint32_t));
        }
    }
    return sizeof words + ((size_t)words * sizeof(uint32_t));
}

static size_t
snapshot_lod_meshes(uint8_t* out, const raster_t* raster, const r3d_instance_t* instances, int count) {
    size_t bytes = 0;
    for (int i = 0; i < count + raster->batch_count; i++) {
        const r3d_batch_t* batch = i < count ? NULL : raster->batches[i - count].batch;
        const r3d_instance_t* instance = i < count ? &instances[i] : &raster->batches[i - count].instance;
        bytes += snapshot_lod_mesh(out != NULL ? out + bytes : NULL, instance, batch);
    }
    return bytes;
}

static bool
reserve_lod_state(size_t bytes) {
    rec->lod_size = 0;
    if (bytes > rec->lod_capacity) {
        memory_free(rec->lod_state);
        rec->lod_state = memory_alloc(bytes, MEMORY_PSRAM);
        rec->lod_capacity = rec->lod_state != NULL ? bytes : 0;
    }
    return rec->lod_state != NULL;
}

static bool
keyframe_due(void) {
    return rec->keyframe_next || (rec->index % rec->keyframe_every == 0);
}

static void
snapshot_lod_state(const frame_record_context_t* context, const raster_t* raster) {
    frame_record_lodstate_t state = {.pick_round = raster->pick_round};
    memcpy(&state.steer, context->steer, sizeof state.steer);
    memcpy(rec->lod_state, &state, sizeof state);
}

void
frame_record_before_draw(const frame_record_context_t* context, const raster_t* raster, const r3d_instance_t* instances,
                         int count) {
    if (rec == NULL || !keyframe_due()) {
        return;
    }
    const size_t header_bytes = sizeof(frame_record_lodstate_t);
    const size_t bytes = header_bytes + snapshot_lod_meshes(NULL, raster, instances, count);
    if (!reserve_lod_state(bytes)) {
        return;
    }
    snapshot_lod_state(context, raster);
    rec->lod_size = header_bytes + snapshot_lod_meshes(rec->lod_state + header_bytes, raster, instances, count);
}

static void
record_lod_state(void) {
    if (rec->lod_size == 0) {
        ESP_LOGE("frame_record", "keyframe has no pre-draw LOD state");
        rec->overflowed = true;
        return;
    }
    record(FRAME_RECORD_LODSTATE, rec->lod_state, rec->lod_size);
    rec->lod_size = 0;
}

void
frame_record_frame(const frame_record_context_t* context, const raster_t* raster, uint32_t dt_ms) {
    if (rec == NULL) {
        return;
    }
    if (context->frame->generated && (rec->after_drop || rec->counts.frames == 0)) {
        rec->index++;
        settle(false, true, false);
        return;
    }
    FRAME_COST_BEGIN(recorded_from);
    const bool generated = context->frame->generated;
    const bool due = keyframe_due();
    /* A generated frame draws no meshes, so it cannot be a keyframe: the next drawn frame is. */
    const bool keyframe = due && !generated;
    rec->size = 0;
    rec->overflowed = false;
    /* A pack mounted or unmounted since the last frame may hold a recorded
     * address now, or the same address in another pack (a scene switch frees
     * a slot and mounts the next scene there): every mesh and pack is
     * recorded again, as at a keyframe. */
    const uint32_t mounts = asset_store_mount_epoch();
    if (mounts != rec->mounts_seen) {
        rec->mounts_seen = mounts;
        memset(rec->slots, 0, RECORDED_SLOTS_MAX * sizeof *rec->slots);
        rec->packs_named = 0;
    }
    if (keyframe) {
        rec->packs_named = 0;
    }
    record_frame_head(context, raster, dt_ms, keyframe);
    if (!generated) {
        record_meshes(raster, keyframe);
        if (keyframe) {
            record_lod_state();
        }
    }
    rec->index++;
    /* The FRAME record leads the bytes: its count of what follows it. */
    const size_t frame_end = sizeof(frame_record_head_t) + sizeof(frame_record_frame_t);
    const uint32_t bytes = (uint32_t)(rec->size - frame_end);
    if (!rec->overflowed) {
        memcpy(rec->bytes + frame_end - sizeof bytes, &bytes, sizeof bytes);
    }
    const bool taken = !rec->overflowed && rec->out.write(rec->out.sink, rec->bytes, rec->size);
    if (!generated) {
        clear_fresh(); /* a generated frame carried no pose or build: the next drawn one does */
    }
    settle(taken, due, keyframe);
    FRAME_COST_END(recorded_from, FRAME_RECORD_STAGE_NAME);
}

void
frame_record_note_shown(const uint16_t* pixels, int width, int height) {
    if (rec == NULL || !rec->check_due || pixels == NULL) {
        return;
    }
    rec->check_due = false;
    rec->check = (frame_record_check_t){rec->index - 1U, (uint16_t)width, (uint16_t)height,
                                        crc32_update(0, pixels, (size_t)width * (size_t)height * sizeof *pixels)};
    rec->checked = true;
}

#endif
