#include "triangle_sizes.h"

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "render/r3d_span_internal.h"

typedef struct {
    float x, y;
} point_t;

static bool
project(const r3d_lens_t* lens, const int16_t p[3], point_t* out) {
    const vec3f_t l = mat4f_apply(&lens->m, (vec3f_t){(float)p[0], (float)p[1], (float)p[2]});
    if (l.z <= lens->near_z) {
        return false;
    }
    *out = (point_t){lens->center_x + (l.x / l.z), lens->center_y + (l.y / l.z)};
    return true;
}

static float
edge(point_t a, point_t b, float px, float py) {
    return ((b.x - a.x) * (py - a.y)) - ((b.y - a.y) * (px - a.x));
}

/* Top and left edges own their centres; the triangle winds positive. */
static bool
owns(point_t a, point_t b, float px, float py) {
    const float w = edge(a, b, px, py);
    const float dy = b.y - a.y;
    return w > 0.0F || (w == 0.0F && (dy < 0.0F || (dy == 0.0F && b.x > a.x)));
}

static int
first_centre(float lo) {
    return (int)ceilf(lo - 0.5F);
}

typedef struct {
    int x0, x1, y0, y1; /* the centres the bounding box holds, half-open */
} box_t;

static box_t
centre_box(const point_t v[3]) {
    return (box_t){
        first_centre(fminf(v[0].x, fminf(v[1].x, v[2].x))),
        first_centre(fmaxf(v[0].x, fmaxf(v[1].x, v[2].x))),
        first_centre(fminf(v[0].y, fminf(v[1].y, v[2].y))),
        first_centre(fmaxf(v[0].y, fmaxf(v[1].y, v[2].y))),
    };
}

static int
clamp_int(int v, int lo, int hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Counts the centres a positive-winding triangle owns, stopping past 4. */
static long
count_centres(const point_t v[3], box_t b, int width, int height) {
    long covered = 0;
    for (int y = clamp_int(b.y0, 0, height); y < clamp_int(b.y1, 0, height) && covered <= 4; y++) {
        for (int x = clamp_int(b.x0, 0, width); x < clamp_int(b.x1, 0, width) && covered <= 4; x++) {
            const float px = (float)x + 0.5F;
            const float py = (float)y + 0.5F;
            covered += owns(v[0], v[1], px, py) && owns(v[1], v[2], px, py) && owns(v[2], v[0], px, py);
        }
    }
    return covered;
}

static void
size_triangle(point_t v[3], bool double_sided, int width, int height, r3d_sizes_t* s) {
    const float area2 = edge(v[0], v[1], v[2].x, v[2].y);
    if (area2 == 0.0F || (!double_sided && area2 > 0.0F)) {
        return; /* front faces wind negative on screen */
    }
    if (area2 < 0.0F) {
        const point_t t = v[1];
        v[1] = v[2];
        v[2] = t;
    }
    const box_t b = centre_box(v);
    if (b.x1 <= 0 || b.x0 >= width || b.y1 <= 0 || b.y0 >= height) {
        return;
    }
    s->drawn++;
    if (b.x0 >= b.x1 || b.y0 >= b.y1) {
        s->box_empty++;
        s->bins[R3D_SIZES_ZERO]++;
        return;
    }
    s->box_two_by_two += (b.x1 - b.x0 <= 2 && b.y1 - b.y0 <= 2);
    const long covered = count_centres(v, b, width, height);
    s->bins[covered == 0   ? R3D_SIZES_ZERO
            : covered == 1 ? R3D_SIZES_ONE
            : covered <= 4 ? R3D_SIZES_TWO_TO_FOUR
                           : R3D_SIZES_MORE]++;
}

void
r3d_sizes_count(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, const uint16_t* visible, int count,
                r3d_sizes_t* out) {
    for (int i = 0; i < count; i++) {
        const r3d_lit_cluster_t* c = &mesh->clusters[visible[i]];
        for (int t = c->triangle_first; t < c->triangle_first + c->triangle_count; t++) {
            point_t v[3];
            bool in_front = true;
            for (int k = 0; k < 3; k++) {
                in_front = in_front && project(lens, mesh->positions[mesh->triangles[t][k]], &v[k]);
            }
            if (in_front) {
                size_triangle(v, c->double_sided, lens->width, lens->height, out);
            }
        }
    }
}

void
r3d_sizes_add(r3d_sizes_t* total, const r3d_sizes_t* s) {
    total->drawn += s->drawn;
    total->box_empty += s->box_empty;
    total->box_two_by_two += s->box_two_by_two;
    for (int b = 0; b < R3D_SIZES_BINS; b++) {
        total->bins[b] += s->bins[b];
    }
}

static int
box_side(int centres) {
    return centres < R3D_BOXES_SIDES ? centres - 1 : R3D_BOXES_SIDES - 1;
}

void
r3d_boxes_count(r3d_boxes_t* out, const r3d_span_target_t* target, const r3d_span_vertex_t* a,
                const r3d_span_vertex_t* b, const r3d_span_vertex_t* c, bool solid) {
    const r3d_span_extent_t e = r3d_span_extent(a, b, c);
    const r3d_span_box_t box = e.centres;
    const bool in_target = box.x0 < target->rows.width && box.x1 > 0 && box.x0 < box.x1 && box.y0 < target->rows.row1
                           && box.y1 > target->rows.row0 && box.y0 < box.y1;
    if (!in_target || r3d_span_area2(a, b, c) == 0) {
        return;
    }
    const int mode = e.flat ? R3D_BOXES_FLAT : (solid ? R3D_BOXES_FACE : R3D_BOXES_SMOOTH);
    out->boxes[mode][box_side(e.centres.y1 - e.centres.y0)][box_side(e.centres.x1 - e.centres.x0)]++;
}

/* Triangles of `mode`, or of every mode for R3D_BOXES_MODES, whose box is
 * at most n x n; all of them for n 0. */
static long
boxes_within(const r3d_boxes_t* boxes, int mode, int n) {
    const int last = n == 0 ? R3D_BOXES_SIDES : n;
    long sum = 0;
    for (int m = 0; m < R3D_BOXES_MODES; m++) {
        for (int rows = 0; rows < last && (m == mode || mode == R3D_BOXES_MODES); rows++) {
            for (int columns = 0; columns < last; columns++) {
                sum += boxes->boxes[m][rows][columns];
            }
        }
    }
    return sum;
}

void
r3d_boxes_print(FILE* f, const r3d_boxes_t* boxes) {
    static const char* const names[R3D_BOXES_MODES + 1] = {"flat", "face", "smooth", "all"};
    (void)fputs("| mode | triangles |", f);
    for (int n = 2; n < R3D_BOXES_SIDES; n++) {
        (void)fprintf(f, " box <= %dx%d |", n, n);
    }
    (void)fputs("\n|---|---:|", f);
    for (int n = 2; n < R3D_BOXES_SIDES; n++) {
        (void)fputs("---:|", f);
    }
    (void)fputs("\n", f);
    for (int mode = 0; mode <= R3D_BOXES_MODES; mode++) {
        const long all = boxes_within(boxes, mode, 0);
        (void)fprintf(f, "| %s | %ld |", names[mode], all);
        for (int n = 2; n < R3D_BOXES_SIDES; n++) {
            (void)fprintf(f, " %.1f%% |", all > 0 ? 100.0 * (double)boxes_within(boxes, mode, n) / (double)all : 0.0);
        }
        (void)fputs("\n", f);
    }
}

/* Reads exactly n numbers from text, and nothing after them. */
static bool
read_floats(const char* text, float* out, int n) {
    for (int i = 0; i < n; i++) {
        char* end;
        out[i] = strtof(text, &end);
        if (end == text) {
            return false;
        }
        text = end;
    }
    return strspn(text, " \t\r\n") == strlen(text);
}

static const char*
read_line(const char* line, r3d_sizes_poses_t* out) {
    float v[6];
    if (strncmp(line, "size ", 5) == 0) {
        if (!read_floats(line + 5, v, 2) || v[0] < 1.0F || v[1] < 1.0F || v[0] > 4096.0F || v[1] > 4096.0F) {
            return "a size line needs a width and a height";
        }
        out->width = (int)v[0];
        out->height = (int)v[1];
    } else if (strncmp(line, "lens ", 5) == 0) {
        if (!read_floats(line + 5, v, 2) || v[0] <= 0.0F || v[1] <= 0.0F) {
            return "a lens line needs a half field of view tangent and a near distance";
        }
        out->half_fov_short_tan = v[0];
        out->near_z = v[1];
    } else if (strncmp(line, "pose ", 5) == 0) {
        if (!read_floats(line + 5, v, 6)) {
            return "a pose line needs an eye and a forward, three numbers each";
        }
        if (out->count >= R3D_SIZES_POSES_MAX) {
            return "more poses than R3D_SIZES_POSES_MAX";
        }
        out->eye[out->count] = (vec3f_t){v[0], v[1], v[2]};
        out->forward[out->count] = (vec3f_t){v[3], v[4], v[5]};
        out->count++;
    } else if (line[strspn(line, " \t\r\n")] != '\0' && line[strspn(line, " \t")] != '#') {
        return "a line is not size, lens, pose or a comment";
    }
    return NULL;
}

bool
r3d_sizes_read_poses(FILE* f, r3d_sizes_poses_t* out, char* problem, size_t problem_size) {
    memset(out, 0, sizeof(*out));
    char line[256];
    for (int number = 1; fgets(line, sizeof line, f) != NULL; number++) {
        const char* wrong = read_line(line, out);
        if (wrong != NULL) {
            (void)snprintf(problem, problem_size, "line %d: %s", number, wrong); /* truncation still reads */
            return false;
        }
    }
    if (out->width == 0 || out->near_z == 0.0F || out->count == 0) {
        (void)snprintf(problem, problem_size, "a poses file needs a size, a lens and at least one pose");
        return false;
    }
    /* Pose files use the source frame; asset/engine_frame.py owns the pack conversion. */
    for (int index = 0; index < out->count; index++) {
        out->eye[index].z = -out->eye[index].z;
        out->forward[index].z = -out->forward[index].z;
    }
    return true;
}

bool
r3d_sizes_read_poses_path(const char* path, r3d_sizes_poses_t* out, char* problem, size_t problem_size) {
    if (strcmp(path, "-") == 0) {
        return r3d_sizes_read_poses(stdin, out, problem, problem_size);
    }
    FILE* f = fopen(path, "r");
    if (f == NULL) {
        (void)snprintf(problem, problem_size, "cannot open it");
        return false;
    }
    const bool read = r3d_sizes_read_poses(f, out, problem, problem_size);
    (void)fclose(f);
    return read;
}
