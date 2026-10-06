/*
 * Skinned-mesh lighting, measured on the host: direct per-vertex N.L against
 * a per-object lookup table indexed by the skinned normal's octahedral cell.
 *
 *   skin_light_bench DATA.bin OUT_DIR
 *
 * DATA.bin is skin_light_data.py's output. Every variant lights every vertex
 * of every frame. OUT_DIR receives three Markdown tables (cost, table build,
 * quality against the reference), sheet.bin, the native RGB565 colour of
 * every vertex of the sheet frame under SHEET_LIGHTS lights, one run of
 * vertices per lit variant, the reference first, and sheet.txt, those
 * variants' short labels, one per line.
 *
 * Every variant skins the normal with the rotation part of the blended joint
 * matrices and ends in the same integer stage (light byte times colour byte,
 * packed to RGB565), so only the light bytes differ. The `ops_t` constants are the
 * float operations each kernel below performs per vertex, counted by hand
 * from its code: `other` is compares, absolute values, sign copies and
 * conversions between float and integer; `int mul` is integer multiplies
 * beyond the shared final stage.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "gfx/gfx_color.h"
#include "util/math/vec3f.h"

#define MAX_JOINTS   64
#define MAX_LIGHTS   8
#define SHEET_LIGHTS 4
#define RUNS         15
#define LUT_MAX      32
/* A larger mesh the table's build is also spread over. */
#define LARGE_MESH   1500

typedef struct {
    vec3f_t normal;
    int8_t normal8[3];
    uint8_t joint[4];
    float weight[4];
    uint8_t colour[3];
} vertex_t;

typedef struct {
    vec3f_t row[3];
} rotation_t;

typedef struct {
    unsigned vertices, joints, frames, sheet_frame;
    vertex_t* vertex;
    rotation_t* rotation; /* frames * joints */
} data_t;

/* Object-space lights: a warm key, a cool fill, a rim and a ground bounce,
 * then four dim accents. Colours are 0..1; the sum can pass 1 and clamps. */
typedef struct {
    vec3f_t dir;
    vec3f_t colour;
} light_t;

static const light_t LIGHTS[MAX_LIGHTS] = {
    {{0.45F, 0.80F, 0.40F}, {0.80F, 0.72F, 0.60F}},   {{-0.70F, 0.35F, 0.30F}, {0.25F, 0.32F, 0.45F}},
    {{0.10F, 0.40F, -0.90F}, {0.35F, 0.35F, 0.30F}},  {{0.00F, -1.00F, 0.15F}, {0.20F, 0.16F, 0.10F}},
    {{0.80F, -0.20F, -0.55F}, {0.12F, 0.10F, 0.14F}}, {{-0.30F, 0.90F, -0.30F}, {0.10F, 0.12F, 0.10F}},
    {{-0.50F, -0.40F, 0.75F}, {0.08F, 0.10F, 0.12F}}, {{0.20F, 0.10F, 0.97F}, {0.10F, 0.09F, 0.08F}},
};
static const vec3f_t AMBIENT = {0.22F, 0.22F, 0.25F};
static const unsigned LIGHT_COUNTS[] = {1, 2, 4, 8};
#define LIGHT_COUNT_N (sizeof(LIGHT_COUNTS) / sizeof(LIGHT_COUNTS[0]))

/* A frame's lights as the kernels take them: unit directions (divided by 127
 * for an int8 normal, whose length is 127), colours and ambient in light-byte
 * units, ambient carrying the +0.5 that rounds the final conversion. */
typedef struct {
    unsigned count;
    vec3f_t dir[MAX_LIGHTS];
    vec3f_t colour[MAX_LIGHTS];
    vec3f_t ambient;
} lights_t;

static lights_t
lights_prepare(unsigned count, float dir_scale) {
    lights_t l = {.count = count};
    for (unsigned i = 0; i < count; i++) {
        l.dir[i] = vec3f_scale(vec3f_normalize(LIGHTS[i].dir), dir_scale);
        l.colour[i] = vec3f_scale(LIGHTS[i].colour, 255.0F);
    }
    l.ambient = vec3f_add(vec3f_scale(AMBIENT, 255.0F), (vec3f_t){0.5F, 0.5F, 0.5F});
    return l;
}

typedef struct {
    unsigned mul, add, div, sqrt, other, imul;
} ops_t;

/* Blend the four joints' rotations by weight (36 mul, 27 add: the first
 * joint seeds the sum) and apply the result to the normal (9 mul, 6 add). A position skin blends the same
 * matrices, so the blend is shared with it in a real path. */
static const ops_t OPS_SKIN = {45, 33, 0, 0, 0, 0};

static inline vec3f_t
skin(const rotation_t* frame, const vertex_t* v, vec3f_t n) {
    const rotation_t* first = &frame[v->joint[0]];
    vec3f_t r0 = vec3f_scale(first->row[0], v->weight[0]);
    vec3f_t r1 = vec3f_scale(first->row[1], v->weight[0]);
    vec3f_t r2 = vec3f_scale(first->row[2], v->weight[0]);
    for (int k = 1; k < 4; k++) {
        const rotation_t* m = &frame[v->joint[k]];
        const float w = v->weight[k];
        r0 = vec3f_add(r0, vec3f_scale(m->row[0], w));
        r1 = vec3f_add(r1, vec3f_scale(m->row[1], w));
        r2 = vec3f_add(r2, vec3f_scale(m->row[2], w));
    }
    return (vec3f_t){vec3f_dot(r0, n), vec3f_dot(r1, n), vec3f_dot(r2, n)};
}

static inline vec3f_t
normal8(const vertex_t* v) {
    return (vec3f_t){(float)v->normal8[0], (float)v->normal8[1], (float)v->normal8[2]};
}

static inline uint8_t
to_byte(float x) {
    return x < 255.0F ? (uint8_t)x : 255;
}

/* Per light: N.L (3 mul, 2 add), the facing test (1 other), colour times N.L
 * added in (3 mul, 3 add), counted as if every light faces the normal; then
 * three clamps and three conversions. */
static const ops_t OPS_LIGHT_BASE = {0, 0, 0, 0, 6, 0};
static const ops_t OPS_PER_LIGHT = {6, 5, 0, 0, 1, 0};

static inline void
light(vec3f_t n, const lights_t* l, uint8_t out[3]) {
    vec3f_t sum = l->ambient;
    for (unsigned i = 0; i < l->count; i++) {
        const float d = vec3f_dot(n, l->dir[i]);
        if (d > 0.0F) {
            sum = vec3f_add(sum, vec3f_scale(l->colour[i], d));
        }
    }
    out[0] = to_byte(sum.x);
    out[1] = to_byte(sum.y);
    out[2] = to_byte(sum.z);
}

/* Renormalising: a dot (3 mul, 2 add), a square root, a reciprocal and three
 * scales. */
static const ops_t OPS_RENORMALISE = {6, 2, 1, 1, 0, 0};

/* Both lookups start from the octahedral point: 3 abs, 2 add, a reciprocal,
 * 2 mul, the hemisphere test, and on the lower half 2 abs, 2 sub, 2 sign
 * copies (2 mul, 4 add, 1 div, 8 other).
 *
 * Nearest: then the cell, 2 add, 2 mul, 2 conversions; the row offset is a
 * shift (power-of-two sizes). */
static const ops_t OPS_NEAREST = {2 + 2, 4 + 2, 1, 0, 8 + 2, 0};

/* Bilinear: then per axis a multiply-add to the cell-centre grid, a clamp
 * (2 compares), the cell and its fraction (2 conversions, a subtract) and the
 * fraction's 8-bit weight (a multiply, a conversion); then per channel three
 * integer lerps of two multiplies each. Row offsets are shifts. */
static const ops_t OPS_BILINEAR = {2 + 4, 4 + 4, 1, 0, 8 + 10, 18};

typedef struct {
    unsigned size, shift; /* size = 1 << shift */
    float half;           /* nearest index scale: just under size / 2, so u = 1 lands in the last cell */
    float centres, bias;  /* bilinear: the cell-centre grid is u * centres + bias */
    float last;           /* bilinear: the last cell's centre on that grid */
    vec3f_t dir[LUT_MAX * LUT_MAX];
    uint8_t rgb[LUT_MAX * LUT_MAX][4];
} lut_t;

static void
lut_init(lut_t* t, unsigned size) {
    t->size = size;
    t->shift = 0;
    while ((1u << t->shift) < size) {
        t->shift++;
    }
    t->half = 0.5F * (float)size * 0.99999F;
    t->centres = 0.5F * (float)size;
    t->bias = t->centres - 0.5F;
    t->last = (float)(size - 1);
    for (unsigned j = 0; j < size; j++) {
        for (unsigned i = 0; i < size; i++) {
            const vec2f_t p = {((float)i + 0.5F) * 2.0F / (float)size - 1.0F,
                               ((float)j + 0.5F) * 2.0F / (float)size - 1.0F};
            t->dir[j * size + i] = vec3f_from_octahedral(p);
        }
    }
}

/* Once per frame per object: every cell lit like a vertex. */
static void
lut_build(lut_t* t, const lights_t* l) {
    const unsigned cells = t->size * t->size;
    for (unsigned c = 0; c < cells; c++) {
        light(t->dir[c], l, t->rgb[c]);
    }
}

static inline const uint8_t*
lut_lookup(const lut_t* t, vec3f_t n) {
    const vec2f_t p = vec3f_octahedral(n);
    const unsigned i = (unsigned)((p.x + 1.0F) * t->half);
    const unsigned j = (unsigned)((p.y + 1.0F) * t->half);
    return t->rgb[(j << t->shift) + i];
}

/* The cell below `u` on the cell-centre grid, clamped so the cell above
 * exists, and the 8-bit weight of the cell above. */
static inline unsigned
lut_axis(const lut_t* t, float u, unsigned* weight) {
    float x = u * t->centres + t->bias;
    x = x < 0.0F ? 0.0F : x;
    x = x > t->last ? t->last : x;
    unsigned i = (unsigned)x;
    i = i > t->size - 2 ? t->size - 2 : i;
    *weight = (unsigned)((x - (float)i) * 256.0F);
    return i;
}

/* The four cells around the point, blended; the border clamps rather than
 * wrapping across the octahedral fold. */
static inline void
lut_bilinear(const lut_t* t, vec3f_t n, uint8_t out[3]) {
    const vec2f_t p = vec3f_octahedral(n);
    unsigned wx, wy;
    const unsigned i = lut_axis(t, p.x, &wx);
    const unsigned j = lut_axis(t, p.y, &wy);
    const uint8_t* a = t->rgb[(j << t->shift) + i];
    const uint8_t* b = a + 4;
    const uint8_t* c = a + 4 * t->size;
    const uint8_t* d = c + 4;
    for (int k = 0; k < 3; k++) {
        const unsigned top = a[k] * (256 - wx) + b[k] * wx;
        const unsigned bottom = c[k] * (256 - wx) + d[k] * wx;
        out[k] = (uint8_t)((top * (256 - wy) + bottom * wy + 32768u) >> 16);
    }
}

typedef enum { K_SKIN, K_REFERENCE, K_DIRECT, K_DIRECT8, K_LUT, K_LUT_BILINEAR } kind_t;

typedef struct {
    const char* name;
    const char* label; /* the sheet's */
    kind_t kind;
    unsigned lut;
} variant_t;

static const variant_t VARIANTS[] = {
    {"Skin only (shared)", "", K_SKIN, 0},
    {"Reference: float normal, renormalised", "Reference", K_REFERENCE, 0},
    {"Direct: float normal", "Direct, float", K_DIRECT, 0},
    {"Direct: int8 normal", "Direct, int8", K_DIRECT8, 0},
    {"Table 8x8 nearest: int8 normal", "8x8 nearest", K_LUT, 8},
    {"Table 16x16 nearest: int8 normal", "16x16 nearest", K_LUT, 16},
    {"Table 32x32 nearest: int8 normal", "32x32 nearest", K_LUT, 32},
    {"Table 8x8 bilinear: int8 normal", "8x8 bilinear", K_LUT_BILINEAR, 8},
    {"Table 16x16 bilinear: int8 normal", "16x16 bilinear", K_LUT_BILINEAR, 16},
    {"Table 32x32 bilinear: int8 normal", "32x32 bilinear", K_LUT_BILINEAR, 32},
};
#define VARIANT_N (sizeof(VARIANTS) / sizeof(VARIANTS[0]))

static int
is_lut(kind_t k) {
    return k == K_LUT || k == K_LUT_BILINEAR;
}

static ops_t
ops_add(ops_t a, ops_t b, unsigned times) {
    return (ops_t){a.mul + b.mul * times,   a.add + b.add * times,     a.div + b.div * times,
                   a.sqrt + b.sqrt * times, a.other + b.other * times, a.imul + b.imul * times};
}

/* An int8 normal's three conversions to float. */
static const ops_t INT8_LOAD = {0, 0, 0, 0, 3, 0};

static ops_t
variant_ops(const variant_t* v, unsigned lights) {
    ops_t lit = ops_add(OPS_LIGHT_BASE, OPS_PER_LIGHT, lights);
    switch (v->kind) {
        case K_SKIN: return OPS_SKIN;
        case K_REFERENCE: return ops_add(ops_add(OPS_SKIN, OPS_RENORMALISE, 1), lit, 1);
        case K_DIRECT: return ops_add(OPS_SKIN, lit, 1);
        case K_DIRECT8: return ops_add(ops_add(OPS_SKIN, lit, 1), INT8_LOAD, 1);
        case K_LUT: return ops_add(ops_add(OPS_SKIN, OPS_NEAREST, 1), INT8_LOAD, 1);
        case K_LUT_BILINEAR: return ops_add(ops_add(OPS_SKIN, OPS_BILINEAR, 1), INT8_LOAD, 1);
    }
    return (ops_t){0, 0, 0, 0, 0, 0};
}

static inline uint16_t
shade(const uint8_t colour[3], const uint8_t l[3]) {
    const uint32_t r = (colour[0] * l[0] + 127u) / 255u;
    const uint32_t g = (colour[1] * l[1] + 127u) / 255u;
    const uint32_t b = (colour[2] * l[2] + 127u) / 255u;
    return (uint16_t)GFX_RGB565((r << 16) | (g << 8) | b);
}

static double
now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

static volatile float sink;

/* Lights every vertex of one frame; returns nothing, writes light bytes. */
static void
run_frame(const data_t* d, const variant_t* v, unsigned frame, const lights_t* lf, const lights_t* l8, const lut_t* lut,
          uint8_t (*out)[3]) {
    const rotation_t* rot = &d->rotation[(size_t)frame * d->joints];
    float acc = 0.0F;
    for (unsigned i = 0; i < d->vertices; i++) {
        const vertex_t* vx = &d->vertex[i];
        switch (v->kind) {
            case K_SKIN: {
                const vec3f_t n = skin(rot, vx, vx->normal);
                acc += n.x + n.y + n.z;
                break;
            }
            case K_REFERENCE: light(vec3f_normalize(skin(rot, vx, vx->normal)), lf, out[i]); break;
            case K_DIRECT: light(skin(rot, vx, vx->normal), lf, out[i]); break;
            case K_DIRECT8: light(skin(rot, vx, normal8(vx)), l8, out[i]); break;
            case K_LUT: memcpy(out[i], lut_lookup(lut, skin(rot, vx, normal8(vx))), 3); break;
            case K_LUT_BILINEAR: lut_bilinear(lut, skin(rot, vx, normal8(vx)), out[i]); break;
        }
    }
    sink = acc;
}

static int
compare_double(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return (x > y) - (x < y);
}

static double
median(double* v, unsigned n) {
    qsort(v, n, sizeof(v[0]), compare_double);
    return v[n / 2];
}

typedef struct {
    double vertex_ns;             /* per vertex, table build excluded */
    double build_ns;              /* per frame */
    double light_max, light_mean; /* light-byte error */
    double rgb_max, rgb_mean;     /* RGB565 error, in 5- or 6-bit steps */
    double rgb_changed;           /* share of vertices whose RGB565 differs */
} result_t;

/* Runs one variant at one light count over every frame RUNS times; keeps the
 * light bytes of every vertex of every frame in `out`. The lights do not move,
 * so one build serves every frame; the builds a real frame loop would do are
 * timed back to back, one per frame. */
static result_t
measure(const data_t* d, const variant_t* v, unsigned lights, uint8_t (*out)[3]) {
    const lights_t lf = lights_prepare(lights, 1.0F);
    const lights_t l8 = lights_prepare(lights, 1.0F / 127.0F);
    static lut_t lut;
    if (is_lut(v->kind)) {
        lut_init(&lut, v->lut);
    }
    double vertex[RUNS], build[RUNS];
    for (unsigned run = 0; run < RUNS; run++) {
        const double t0 = now_ns();
        for (unsigned f = 0; is_lut(v->kind) && f < d->frames; f++) {
            lut_build(&lut, &lf);
        }
        const double t1 = now_ns();
        for (unsigned f = 0; f < d->frames; f++) {
            run_frame(d, v, f, &lf, &l8, &lut, &out[(size_t)f * d->vertices]);
        }
        vertex[run] = (now_ns() - t1) / ((double)d->frames * d->vertices);
        build[run] = (t1 - t0) / d->frames;
    }
    result_t r = {0};
    r.vertex_ns = median(vertex, RUNS);
    r.build_ns = is_lut(v->kind) ? median(build, RUNS) : 0.0;
    return r;
}

static void
score(const data_t* d, uint8_t (*got)[3], uint8_t (*ref)[3], result_t* r) {
    const size_t n = (size_t)d->frames * d->vertices;
    double light_sum = 0.0, rgb_sum = 0.0;
    size_t changed = 0;
    for (size_t k = 0; k < n; k++) {
        const vertex_t* vx = &d->vertex[k % d->vertices];
        for (int c = 0; c < 3; c++) {
            const double e = abs((int)got[k][c] - (int)ref[k][c]);
            light_sum += e;
            if (e > r->light_max) {
                r->light_max = e;
            }
        }
        const uint16_t a = shade(vx->colour, got[k]), b = shade(vx->colour, ref[k]);
        const int e[3] = {abs((int)gfx_rgb565_r5(a) - (int)gfx_rgb565_r5(b)),
                          abs((int)gfx_rgb565_g6(a) - (int)gfx_rgb565_g6(b)),
                          abs((int)gfx_rgb565_b5(a) - (int)gfx_rgb565_b5(b))};
        for (int c = 0; c < 3; c++) {
            rgb_sum += e[c];
            if (e[c] > r->rgb_max) {
                r->rgb_max = e[c];
            }
        }
        changed += a != b;
    }
    r->light_mean = light_sum / (3.0 * (double)n);
    r->rgb_mean = rgb_sum / (3.0 * (double)n);
    r->rgb_changed = 100.0 * (double)changed / (double)n;
}

static int
load(const char* path, data_t* d) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        return 0;
    }
    char magic[4];
    uint32_t head[4];
    int ok = fread(magic, 1, 4, f) == 4 && memcmp(magic, "SKLT", 4) == 0 && fread(head, 4, 4, f) == 4;
    if (ok) {
        d->vertices = head[0];
        d->joints = head[1];
        d->frames = head[2];
        d->sheet_frame = head[3];
        ok = d->joints <= MAX_JOINTS && d->sheet_frame < d->frames;
    }
    if (ok) {
        d->vertex = calloc(d->vertices, sizeof(vertex_t));
        d->rotation = calloc((size_t)d->frames * d->joints, sizeof(rotation_t));
        ok = d->vertex && d->rotation;
    }
    for (unsigned i = 0; ok && i < d->vertices; i++) {
        float n[3], w[4];
        uint8_t j[4], c[4];
        ok = fread(n, 4, 3, f) == 3 && fread(j, 1, 4, f) == 4 && fread(w, 4, 4, f) == 4 && fread(c, 1, 4, f) == 4;
        vertex_t* v = &d->vertex[i];
        v->normal = (vec3f_t){n[0], n[1], n[2]};
        for (int k = 0; k < 4; k++) {
            ok = ok && j[k] < d->joints;
            v->joint[k] = j[k];
            v->weight[k] = w[k];
        }
        for (int k = 0; k < 3; k++) {
            v->normal8[k] = (int8_t)lroundf(n[k] * 127.0F);
            v->colour[k] = c[k];
        }
    }
    for (size_t m = 0; ok && m < (size_t)d->frames * d->joints; m++) {
        float r[9];
        ok = fread(r, 4, 9, f) == 9;
        for (int k = 0; k < 3; k++) {
            d->rotation[m].row[k] = (vec3f_t){r[k * 3], r[k * 3 + 1], r[k * 3 + 2]};
        }
    }
    fclose(f);
    return ok;
}

static FILE*
open_out(const char* dir, const char* name, const char* mode) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, mode);
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
    return f;
}

static void
ops_cell(FILE* f, ops_t o) {
    fprintf(f, " %u | %u | %u | %u | %u | %u |", o.mul, o.add, o.div, o.sqrt, o.other, o.imul);
}

int
main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s DATA.bin OUT_DIR\n", argv[0]);
        return 2;
    }
    data_t d;
    if (!load(argv[1], &d)) {
        fprintf(stderr, "%s: not a skin_light_data.py file\n", argv[1]);
        return 1;
    }
    const size_t n = (size_t)d.frames * d.vertices;
    uint8_t(*ref)[3] = malloc(n * 3);
    uint8_t(*got)[3] = malloc(n * 3);
    uint16_t* sheet = malloc(VARIANT_N * d.vertices * sizeof(uint16_t));
    static result_t results[VARIANT_N][LIGHT_COUNT_N];
    unsigned reference = 0;
    while (VARIANTS[reference].kind != K_REFERENCE) {
        reference++;
    }

    /* The reference's sheet run goes first, the others in VARIANTS order. */
    const char* labels[VARIANT_N];
    unsigned sheet_runs = 1;
    for (unsigned li = 0; li < LIGHT_COUNT_N; li++) {
        results[reference][li] = measure(&d, &VARIANTS[reference], LIGHT_COUNTS[li], ref);
        for (unsigned vi = 0; vi < VARIANT_N; vi++) {
            if (vi != reference) {
                results[vi][li] = measure(&d, &VARIANTS[vi], LIGHT_COUNTS[li], got);
            }
            if (VARIANTS[vi].kind == K_SKIN) {
                continue;
            }
            uint8_t(*mine)[3] = vi == reference ? ref : got;
            score(&d, mine, ref, &results[vi][li]);
            if (LIGHT_COUNTS[li] == SHEET_LIGHTS) {
                const unsigned run = vi == reference ? 0 : sheet_runs++;
                for (unsigned i = 0; i < d.vertices; i++) {
                    sheet[run * d.vertices + i] =
                        shade(d.vertex[i].colour, mine[(size_t)d.sheet_frame * d.vertices + i]);
                }
                labels[run] = VARIANTS[vi].label;
            }
        }
    }
    FILE* f = open_out(argv[2], "sheet.txt", "w");
    for (unsigned run = 0; run < sheet_runs; run++) {
        fprintf(f, "%s\n", labels[run]);
    }
    fclose(f);

    f = open_out(argv[2], "skin-light-cost.md", "w");
    fprintf(f, "%u vertices x %u frames, median of %u runs; host compiler " __VERSION__ ".\n\n", d.vertices, d.frames,
            RUNS);
    fprintf(f, "| Variant | Lights | mul | add | div | sqrt | other | int mul | ns/vertex |\n");
    fprintf(f, "|---|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (unsigned vi = 0; vi < VARIANT_N; vi++) {
        for (unsigned li = 0; li < LIGHT_COUNT_N; li++) {
            const int any = VARIANTS[vi].kind == K_SKIN || is_lut(VARIANTS[vi].kind);
            if (any && li > 0) {
                continue;
            }
            if (any) {
                fprintf(f, "| %s | any |", VARIANTS[vi].name);
            } else {
                fprintf(f, "| %s | %u |", VARIANTS[vi].name, LIGHT_COUNTS[li]);
            }
            ops_cell(f, variant_ops(&VARIANTS[vi], LIGHT_COUNTS[li]));
            fprintf(f, " %.1f |\n", results[vi][li].vertex_ns);
        }
    }
    fclose(f);

    f = open_out(argv[2], "skin-light-build.md", "w");
    fprintf(f,
            "| Table | Cells | Bytes per object | Shared direction bytes | Lights | mul | add | div | sqrt | other | "
            "int mul "
            "| Build us/frame | Build ns/vertex, %u vertices | Build ns/vertex, %u vertices |\n",
            d.vertices, LARGE_MESH);
    fprintf(f, "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|\n");
    for (unsigned vi = 0; vi < VARIANT_N; vi++) {
        if (VARIANTS[vi].kind != K_LUT) {
            continue;
        }
        const unsigned cells = VARIANTS[vi].lut * VARIANTS[vi].lut;
        for (unsigned li = 0; li < LIGHT_COUNT_N; li++) {
            const double b = results[vi][li].build_ns;
            fprintf(f, "| %ux%u | %u | %u | %u | %u |", VARIANTS[vi].lut, VARIANTS[vi].lut, cells, cells * 4,
                    cells * 12, LIGHT_COUNTS[li]);
            const ops_t o = ops_add(OPS_LIGHT_BASE, OPS_PER_LIGHT, LIGHT_COUNTS[li]);
            ops_cell(f, (ops_t){o.mul * cells, o.add * cells, 0, 0, o.other * cells, 0});
            fprintf(f, " %.2f | %.1f | %.1f |\n", b / 1000.0, b / d.vertices, b / LARGE_MESH);
        }
    }
    fclose(f);

    f = open_out(argv[2], "skin-light-quality.md", "w");
    fprintf(f, "| Variant | Lights | Light byte max | Light byte mean | RGB565 max step | RGB565 mean step "
               "| Vertices changed |\n");
    fprintf(f, "|---|---:|---:|---:|---:|---:|---:|\n");
    for (unsigned vi = 0; vi < VARIANT_N; vi++) {
        for (unsigned li = 0; li < LIGHT_COUNT_N && VARIANTS[vi].kind != K_SKIN && vi != reference; li++) {
            const result_t* r = &results[vi][li];
            fprintf(f, "| %s | %u | %.0f | %.2f | %.0f | %.3f | %.1f%% |\n", VARIANTS[vi].name, LIGHT_COUNTS[li],
                    r->light_max, r->light_mean, r->rgb_max, r->rgb_mean, r->rgb_changed);
        }
    }
    fclose(f);

    f = open_out(argv[2], "sheet.bin", "wb");
    fwrite(sheet, sizeof(uint16_t), (size_t)sheet_runs * d.vertices, f);
    fclose(f);
    printf("wrote %s\n", argv[2]);
    return 0;
}
