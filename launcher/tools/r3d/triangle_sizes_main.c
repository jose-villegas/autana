/*
 * The triangle_sizes tool: a lit mesh's triangles by the pixel centres they
 * cover at each pose of a poses file or standard input, and each pose's rendered frame kept
 * or compared. The mesh is linked in by report_triangle_sizes.sh, which
 * names its symbol as R3D_SIZES_MESH.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "render/r3d_pipeline.h"
#include "triangle_sizes.h"

#ifndef R3D_SIZES_MESH
#error "build with -DR3D_SIZES_MESH=<the mesh's symbol>"
#endif

extern const r3d_lit_mesh_t R3D_SIZES_MESH;

typedef struct {
    int width, height;
    size_t pixels;
} frame_size_t;

static void*
checked_malloc(size_t bytes) {
    void* p = malloc(bytes);
    if (p == NULL) {
        (void)fputs("out of memory\n", stderr);
        exit(1);
    }
    return p;
}

static int
channel_gap(uint16_t a, uint16_t b) {
    const unsigned na = (unsigned)((a >> 8) | (a << 8)) & 0xFFFFU;
    const unsigned nb = (unsigned)((b >> 8) | (b << 8)) & 0xFFFFU;
    const int dr = abs((int)(na >> 11) - (int)(nb >> 11)) * 8;
    const int dg = abs((int)((na >> 5) & 63U) - (int)((nb >> 5) & 63U)) * 4;
    const int db = abs((int)(na & 31U) - (int)(nb & 31U)) * 8;
    return dr > dg ? (dr > db ? dr : db) : (dg > db ? dg : db);
}

static void
write_frame(FILE* f, const char* path, const uint16_t* color, frame_size_t size) {
    const size_t wrote = fwrite(color, sizeof(*color), size.pixels, f);
    if (fclose(f) != 0 || wrote != size.pixels) {
        (void)fprintf(stderr, "cannot write %s\n", path);
        exit(1);
    }
}

static void
compare_frame(FILE* f, const char* path, int pose, const uint16_t* color, frame_size_t size) {
    uint16_t* before = checked_malloc(size.pixels * sizeof(*before));
    const size_t got = fread(before, sizeof(*before), size.pixels, f);
    const bool longer = fgetc(f) != EOF;
    (void)fclose(f);
    if (got != size.pixels || longer) {
        (void)fprintf(stderr, "%s is not a %dx%d frame\n", path, size.width, size.height);
        exit(1);
    }
    long differ = 0;
    int worst = 0;
    for (size_t i = 0; i < size.pixels; i++) {
        if (before[i] != color[i]) {
            differ++;
            const int gap = channel_gap(before[i], color[i]);
            worst = gap > worst ? gap : worst;
        }
    }
    (void)printf("  pose %2d: %ld of %zu pixels differ (%.3f%%), largest channel gap %d of 255\n", pose, differ,
                 size.pixels, 100.0 * (double)differ / (double)size.pixels, worst);
    free(before);
}

/* Writes the frame to dir/pose_NN.raw, or with `compare` reports how it
 * differs from the one already there. */
static void
keep_frame(const char* dir, bool compare, int pose, const uint16_t* color, frame_size_t size) {
    char path[512];
    const int length = snprintf(path, sizeof path, "%s/pose_%02d.raw", dir, pose);
    if (length < 0 || (size_t)length >= sizeof path) {
        (void)fputs("frame directory path too long\n", stderr);
        exit(1);
    }
    FILE* f = fopen(path, compare ? "rb" : "wb");
    if (f == NULL) {
        (void)fprintf(stderr, "cannot open %s\n", path);
        exit(1);
    }
    if (compare) {
        compare_frame(f, path, pose, color, size);
    } else {
        write_frame(f, path, color, size);
    }
}

static void
print_sizes(int pose, const r3d_sizes_t* s) {
    const double n = s->drawn > 0 ? (double)s->drawn : 1.0;
    char label[16];
    if (snprintf(label, sizeof label, pose < 0 ? "all" : "pose %d", pose) < 0) {
        exit(1);
    }
    (void)printf(
        "%-8s drawn %6ld | 0: %5.1f%% (box empty %5.1f%%) | 1: %5.1f%% | 2-4: %5.1f%% | >4: %5.1f%% | box <= 2x2: "
        "%5.1f%%\n",
        label, s->drawn, 100.0 * (double)s->bins[R3D_SIZES_ZERO] / n, 100.0 * (double)s->box_empty / n,
        100.0 * (double)s->bins[R3D_SIZES_ONE] / n, 100.0 * (double)s->bins[R3D_SIZES_TWO_TO_FOUR] / n,
        100.0 * (double)s->bins[R3D_SIZES_MORE] / n, 100.0 * (double)s->box_two_by_two / n);
}

static void
read_poses_or_exit(const char* path, r3d_sizes_poses_t* poses) {
    char problem[160];
    if (!r3d_sizes_read_poses_path(path, poses, problem, sizeof problem)) {
        (void)fprintf(stderr, "%s: %s\n", path, problem);
        exit(1);
    }
}

typedef struct {
    uint16_t* visible;
    r3d_pipeline_rows_t* rows;
    r3d_pipeline_vertex_t* cs;
    uint16_t* color;
    uint16_t* depth;
} buffers_t;

static void
render_pose(const r3d_lit_mesh_t* mesh, const r3d_lens_t* lens, int count, const buffers_t* b, frame_size_t size) {
    memset(b->color, 0, size.pixels * sizeof(*b->color));
    memset(b->depth, 0, size.pixels * sizeof(*b->depth));
    r3d_pipeline_transform(mesh, lens, b->visible, count, b->cs, b->rows);
    const r3d_span_target_t target = {b->color, b->depth, size.width, 0, size.height};
    r3d_pipeline_draw(mesh, lens, b->visible, count, b->cs, b->rows, &target);
}

int
main(int argc, char** argv) {
    const bool flag_known = argc == 4 && (strcmp(argv[2], "--write") == 0 || strcmp(argv[2], "--against") == 0);
    if (argc != 2 && !flag_known) {
        (void)fputs("usage: triangle_sizes POSES|- [--write DIR | --against DIR]\n", stderr);
        return 2;
    }
    const char* dir = argc == 4 ? argv[3] : NULL;
    const bool compare = argc == 4 && strcmp(argv[2], "--against") == 0;
    r3d_sizes_poses_t* poses = checked_malloc(sizeof(*poses));
    read_poses_or_exit(argv[1], poses);

    const r3d_lit_mesh_t* mesh = &R3D_SIZES_MESH;
    const frame_size_t size = {poses->width, poses->height, (size_t)poses->width * (size_t)poses->height};
    const buffers_t b = {
        checked_malloc(sizeof(uint16_t) * (size_t)mesh->cluster_count),
        checked_malloc(sizeof(r3d_pipeline_rows_t) * (size_t)mesh->cluster_count),
        checked_malloc(sizeof(r3d_pipeline_vertex_t) * (size_t)mesh->vertex_count),
        checked_malloc(size.pixels * sizeof(uint16_t)),
        checked_malloc(size.pixels * sizeof(uint16_t)),
    };
    (void)printf("%d triangles, rendered %dx%d; triangles by pixel centres covered\n", mesh->triangle_count, size.width,
                 size.height);
    r3d_sizes_t total = {0};
    for (int pose = 0; pose < poses->count; pose++) {
        r3d_lens_t lens;
        r3d_lens_init(&lens,
                      &(camera_t){poses->eye[pose], poses->forward[pose], poses->half_fov_short_tan, poses->near_z},
                      mesh->position_scale, (viewport_t){size.width, size.height, 0});
        const int count = r3d_pipeline_cull(mesh, &lens, b.visible);
        r3d_sizes_t s = {0};
        r3d_sizes_count(mesh, &lens, b.visible, count, &s);
        print_sizes(pose, &s);
        r3d_sizes_add(&total, &s);
        render_pose(mesh, &lens, count, &b, size);
        if (dir != NULL) {
            keep_frame(dir, compare, pose, b.color, size);
        }
    }
    print_sizes(-1, &total);
    free(poses);
    free(b.visible);
    free(b.rows);
    free(b.cs);
    free(b.color);
    free(b.depth);
    return 0;
}
