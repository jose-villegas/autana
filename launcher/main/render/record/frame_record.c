/* frame_record: see frame_record.h; the bytes are frame_record_format.h's. */
#include "render/record/frame_record.h"

#include <string.h>

#include "asset/asset_store.h"
#include "core/memory.h"
#include "core/tune.h"
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
    uint32_t noted_at;
} note_head_t;

typedef struct {
    note_head_t head; /* the posed positions */
    const void* bind_positions;
    int vertex_count;
    const mat4f_t* palette;
    uint16_t joints;
    bool fresh; /* posed since the last recorded frame */
} noted_skin_t;

typedef struct {
    note_head_t head; /* the built positions */
    int vertex_count;
    int position_scale;
    r3d_dynamic_source_t source;
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

static noted_skin_t skins[FRAME_RECORD_NOTED_SKINS_MAX];
static noted_built_t built[FRAME_RECORD_NOTED_BUILT_MAX];
static uint32_t notes_made;

typedef struct {
    frame_record_out_t out;
    uint32_t keyframe_every;
    uint32_t index;
    bool keyframe_next;
    bool after_drop;
    uint8_t* bytes;
    size_t size, capacity;
    bool overflowed;
    recorded_slot_t* slots;
    uint32_t packs_named; /* bit per asset_store slot, since the last keyframe */
    int32_t* tuned;       /* each tunable's value as last recorded, in registry order */
    int tunables;
    bool check_due; /* the last frame was a keyframe the sink took: hash what composes from it */
    bool checked;   /* `check` holds a hash for the next frame's records */
    frame_record_check_t check;
    frame_record_counts_t counts;
} recording_t;

static recording_t* rec;

/* The note keyed `key` in a table of `count` notes `stride` apart, else an
 * empty one, else the oldest. */
static void*
note_slot(void* table, size_t stride, int count, const void* key) {
    note_head_t* slot = table;
    for (int i = 0; i < count; i++) {
        note_head_t* note = (note_head_t*)((uint8_t*)table + ((size_t)i * stride));
        if (note->key == key) {
            return note;
        }
        if (slot->key != NULL && (note->key == NULL || note->noted_at < slot->noted_at)) {
            slot = note;
        }
    }
    return slot;
}

void
frame_record_note_skin(const r3d_lit_mesh_t* posed, const r3d_lit_mesh_t* bind, const mat4f_t* palette, int joints) {
    noted_skin_t* slot = note_slot(skins, sizeof skins[0], FRAME_RECORD_NOTED_SKINS_MAX, posed->positions);
    *slot = (noted_skin_t){
        {posed->positions, ++notes_made}, bind->positions, bind->vertex_count, palette, (uint16_t)joints, true};
}

void
frame_record_note_built(const r3d_lit_mesh_t* mesh, const r3d_dynamic_source_t* source, bool fitted) {
    noted_built_t* slot = note_slot(built, sizeof built[0], FRAME_RECORD_NOTED_BUILT_MAX, mesh->positions);
    *slot = (noted_built_t){
        {mesh->positions, ++notes_made}, mesh->vertex_count, mesh->position_scale, *source, fitted, true};
}

static bool
inside(const void* address, const void* block, size_t bytes) {
    return (const uint8_t*)address >= (const uint8_t*)block && (const uint8_t*)address < (const uint8_t*)block + bytes;
}

void
frame_record_forget(const void* block, size_t bytes) {
    for (int i = 0; i < FRAME_RECORD_NOTED_SKINS_MAX; i++) {
        if (inside(skins[i].head.key, block, bytes)) {
            skins[i] = (noted_skin_t){0};
        }
    }
    for (int i = 0; i < FRAME_RECORD_NOTED_BUILT_MAX; i++) {
        if (inside(built[i].head.key, block, bytes)) {
            built[i] = (noted_built_t){0};
        }
    }
}

#if BUILD_VARIANT_SELFTEST_OR_HOST
void
frame_record_forget_notes(void) {
    memset(skins, 0, sizeof skins);
    memset(built, 0, sizeof built);
}
#endif

static noted_skin_t*
skin_of(const r3d_lit_mesh_t* mesh) {
    for (int i = 0; i < FRAME_RECORD_NOTED_SKINS_MAX; i++) {
        if (skins[i].head.key == mesh->positions && skins[i].vertex_count == mesh->vertex_count) {
            return &skins[i];
        }
    }
    return NULL;
}

static noted_built_t*
built_of(const r3d_lit_mesh_t* mesh) {
    for (int i = 0; i < FRAME_RECORD_NOTED_BUILT_MAX; i++) {
        if (built[i].head.key == mesh->positions && built[i].vertex_count == mesh->vertex_count) {
            return &built[i];
        }
    }
    return NULL;
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
        return (frame_record_where_t){FRAME_RECORD_NO_PACK, FRAME_RECORD_NO_ENTRY};
    }
    if ((rec->packs_named & (1U << at.slot)) == 0) {
        frame_record_pack_t pack = {.slot = (uint8_t)at.slot, .crc32 = at.pack_crc};
        (void)strncpy(pack.name, at.pack_name, sizeof pack.name - 1);
        record(FRAME_RECORD_PACK, &pack, sizeof pack);
        rec->packs_named |= 1U << at.slot;
    }
    return (frame_record_where_t){(uint8_t)at.slot, at.entry};
}

static bool
moved(int index, const r3d_lit_mesh_t* mesh, const void* batch, const mat4f_t* model) {
    if (index >= RECORDED_SLOTS_MAX) {
        return true;
    }
    recorded_slot_t* slot = &rec->slots[index];
    const recorded_slot_t now = {
        mesh, mesh->positions, mesh->colors, batch, model != NULL ? *model : mat4f_identity(), model != NULL};
    const bool same = slot->mesh == now.mesh && slot->positions == now.positions && slot->colours == now.colours
                      && slot->batch == now.batch && slot->placed == now.placed
                      && memcmp(&slot->model, &now.model, sizeof now.model) == 0;
    *slot = now;
    return !same;
}

static void
record_pose(int index, noted_skin_t* skin, bool keyframe) {
    if (!keyframe && !skin->fresh) {
        return;
    }
    const frame_record_pose_t pose = {(uint16_t)index, skin->joints};
    head(FRAME_RECORD_POSE, sizeof pose + ((size_t)skin->joints * sizeof(mat4f_t)));
    put(&pose, sizeof pose);
    put(skin->palette, (size_t)skin->joints * sizeof(mat4f_t));
}

static void
record_built(int index, noted_built_t* b, bool keyframe) {
    if (!keyframe && !b->fresh) {
        return;
    }
    const r3d_dynamic_source_t* s = &b->source;
    const size_t positions = (size_t)s->vertex_count * sizeof s->positions[0];
    const size_t colours = (size_t)s->vertex_count * sizeof s->colors[0];
    const size_t triangles = (size_t)s->triangle_count * sizeof s->triangles[0];
    const frame_record_dynmesh_t mesh = {(uint16_t)index,           b->fitted,
                                         s->double_sided,           b->position_scale,
                                         (uint32_t)s->vertex_count, (uint32_t)s->triangle_count};
    head(FRAME_RECORD_DYNMESH, sizeof mesh + positions + colours + triangles);
    put(&mesh, sizeof mesh);
    put(s->positions, positions);
    put(s->colors, colours);
    put(s->triangles, triangles);
}

/* After a frame: every instance drawing a fresh mesh has carried it. */
static void
clear_fresh(void) {
    for (int i = 0; i < FRAME_RECORD_NOTED_SKINS_MAX; i++) {
        skins[i].fresh = false;
    }
    for (int i = 0; i < FRAME_RECORD_NOTED_BUILT_MAX; i++) {
        built[i].fresh = false;
    }
}

static void
record_mesh(int index, const r3d_instance_t* instance, const r3d_batch_t* batch, bool keyframe) {
    const r3d_lit_mesh_t* mesh = instance->mesh;
    noted_skin_t* skin = skin_of(mesh);
    noted_built_t* b = skin == NULL ? built_of(mesh) : NULL;
    const bool changed = moved(index, mesh, batch, instance->model);
    if (keyframe || changed) {
        frame_record_mesh_t m = {.index = (uint16_t)index, .placed = instance->model != NULL};
        m.kind = batch != NULL  ? FRAME_RECORD_BATCH
                 : skin != NULL ? FRAME_RECORD_SKINNED
                 : b != NULL    ? FRAME_RECORD_BUILT
                                : FRAME_RECORD_STATIC;
        m.positions = where(skin != NULL ? skin->bind_positions : (const void*)mesh->positions);
        m.colours = where(mesh->colors);
        m.batch = where(batch != NULL ? (const void*)batch->places : NULL);
        const mat4f_t model = instance->model != NULL ? *instance->model : mat4f_identity();
        memcpy(m.model, &model, sizeof m.model);
        record(FRAME_RECORD_MESH, &m, sizeof m);
    }
    if (skin != NULL) {
        record_pose(index, skin, keyframe);
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

void
frame_record_frame(const render_context_t* context, const raster_t* raster, uint32_t dt_ms) {
    if (rec == NULL) {
        return;
    }
    FRAME_COST_BEGIN(recorded_from);
    const bool generated = context->frame.generated;
    const bool keyframe_due = rec->keyframe_next || rec->index % rec->keyframe_every == 0;
    /* A generated frame draws no meshes, so it cannot be a keyframe: the next drawn frame is. */
    const bool keyframe = keyframe_due && !generated;
    rec->size = 0;
    rec->overflowed = false;
    if (keyframe) {
        rec->packs_named = 0;
    }
    const frame_record_frame_t frame = {
        .index = rec->index,
        .dt_ms = dt_ms,
        .flags = (uint8_t)((keyframe ? FRAME_RECORD_KEYFRAME : 0U) | (rec->after_drop ? FRAME_RECORD_AFTER_DROP : 0U)
                           | (generated ? FRAME_RECORD_GENERATED : 0U)),
        .instances = generated ? 0U : (uint16_t)raster->instance_count,
        .batches = generated ? 0U : (uint16_t)raster->batch_count,
    };
    record(FRAME_RECORD_FRAME, &frame, sizeof frame);
    record(FRAME_RECORD_VIEW, &context->frame_view, sizeof context->frame_view);
    const frame_record_choice_t choice = {(int16_t)context->frame.step, generated, (uint8_t)context->scale_percent,
                                          raster->clear};
    record(FRAME_RECORD_CHOICE, &choice, sizeof choice);
    if (rec->checked) {
        record(FRAME_RECORD_CHECK, &rec->check, sizeof rec->check);
    }
    record_tunables(keyframe);
    if (!generated) {
        for (int i = 0; i < raster->instance_count; i++) {
            record_mesh(i, &raster->instances[i], NULL, keyframe);
        }
        for (int i = 0; i < raster->batch_count; i++) {
            record_mesh(raster->instance_count + i, &raster->batches[i].instance, raster->batches[i].batch, keyframe);
        }
    }
    rec->index++;
    const bool taken = !rec->overflowed && rec->out.write(rec->out.sink, rec->bytes, rec->size);
    if (taken) {
        rec->counts.frames++;
    } else {
        rec->counts.dropped++;
    }
    if (!generated) {
        clear_fresh(); /* a generated frame carried no pose or build: the next drawn one does */
    }
    rec->checked = rec->checked && !taken;
    rec->keyframe_next = !taken || (keyframe_due && !keyframe);
    rec->after_drop = !taken;
    rec->check_due = taken && keyframe;
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
