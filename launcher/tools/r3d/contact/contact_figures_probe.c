/* contact_figures_probe: ground-footprint coverage from the runtime rasterizer. */

#include <stdio.h>
#include <stdlib.h>

#include "render/contact/contact_blob.h"
#include "scene/scene.h"
#include "scene/skin/skin_contact.h"

#define DISC_SIDE   72
#define DISC_FOCAL  72.0F
#define DISC_DEPTH  4.0F
#define DISC_RADIUS 1.75F
#define WHITE       UINT16_MAX
#define BODY_HALF_X 1.5F
#define BODY_HALF_Z 0.6F
#define FOOT_X      1.0F
#define FOOT_Z      0.55F

static const float DISC_STRENGTHS[] = {0.25F, 0.5F, SCENE_CONTACT_BLOB_STRENGTH_DEFAULT, 1.0F};
#define DISC_COUNT ((int)(sizeof DISC_STRENGTHS / sizeof DISC_STRENGTHS[0]))

static void
print_coverage(const r3d_span_target_t* target, const r3d_lens_t* lens, const contact_blob_t* footprints, int count,
               void* work) {
    for (int y = 0; y < DISC_SIDE; y++) {
        uint16_t* row = gfx_target_color(&target->rows, y);
        for (int x = 0; x < DISC_SIDE; x++) {
            row[x] = WHITE;
        }
    }
    contact_blobs_draw(target, lens, footprints, count, work);
    printf("[");
    for (int y = 0; y < DISC_SIDE; y++) {
        const uint16_t* row = gfx_target_color(&target->rows, y);
        for (int x = 0; x < DISC_SIDE; x++) {
            printf("%s%d", x || y ? "," : "", row[x] != WHITE);
        }
    }
    printf("]");
}

static void
print_discs(const r3d_span_target_t* target, const r3d_lens_t* lens, void* work) {
    printf("\"disc\": {\"radius_px\": %g, \"side\": %d, \"strengths\": [", DISC_RADIUS * DISC_FOCAL / DISC_DEPTH,
           DISC_SIDE);
    for (int s = 0; s < DISC_COUNT; s++) {
        printf("%s%g", s ? ", " : "", DISC_STRENGTHS[s]);
    }
    printf("], \"default\": %g, \"covered\": [", SCENE_CONTACT_BLOB_STRENGTH_DEFAULT);
    for (int s = 0; s < DISC_COUNT; s++) {
        const contact_blob_t disc = {.u = {DISC_RADIUS, 0, 0}, .v = {0, 0, DISC_RADIUS}, .strength = DISC_STRENGTHS[s]};
        printf("%s", s ? ", " : "");
        print_coverage(target, lens, &disc, 1, work);
    }
    printf("]}");
}

static void
print_shapes(const r3d_span_target_t* target, const r3d_lens_t* lens, void* work) {
    static const vec3f_t feet[] = {
        {-FOOT_X, 0, -FOOT_Z}, {-FOOT_X, 0, FOOT_Z}, {FOOT_X, 0, -FOOT_Z}, {FOOT_X, 0, FOOT_Z}};
    const float radius = CONTACT_RADIUS_FRACTION * (2 * FOOT_Z);
    const float strength = SCENE_CONTACT_BLOB_STRENGTH_DEFAULT;
    const contact_blob_t disc = {.u = {BODY_HALF_Z, 0, 0}, .v = {0, 0, BODY_HALF_Z}, .strength = strength};
    printf(",\"shapes\": {\"BLOB\": ");
    print_coverage(target, lens, &disc, 1, work);

    enum { FOOT_COUNT = sizeof feet / sizeof feet[0] };

    contact_blob_t footprints[FOOT_COUNT];
    for (int i = 0; i < FOOT_COUNT; i++) {
        footprints[i] =
            (contact_blob_t){.centre = feet[i], .u = {radius, 0, 0}, .v = {0, 0, radius}, .strength = strength};
    }
    printf(", \"FEET\": ");
    print_coverage(target, lens, footprints, FOOT_COUNT, work);

    static const contact_joint_t joints[] = {
        {{-0.75F, 0, 0}, {0.75F, 0, BODY_HALF_Z}, 1},
        {{0.45F, 0, 0}, {0.9F, 0, BODY_HALF_Z}, 1},
        {{1.15F, 0, 0.15F}, {0.35F, 0, 0.35F}, 1},
    };
    const mat4f_t world = mat4f_identity();
    const mat4f_t palette[] = {world, world, world};
    const box3f_t bounds = {{-BODY_HALF_X, 0, -BODY_HALF_Z}, {BODY_HALF_X, 0, BODY_HALF_Z}};
    uint8_t coverage[CONTACT_MASK_BYTES];
    contact_mask_t mask = {.coverage = coverage};
    contact_mask_build(&mask, joints, sizeof joints / sizeof joints[0], palette, 1, &world, bounds);
    const contact_blob_t silhouette = {
        .u = {BODY_HALF_X, 0, 0}, .v = {0, 0, BODY_HALF_Z}, .strength = strength, .mask = &mask};
    printf(", \"SILHOUETTE\": ");
    print_coverage(target, lens, &silhouette, 1, work);
    printf("}");
}

int
main(void) {
    uint16_t* color = malloc(DISC_SIDE * DISC_SIDE * sizeof *color);
    uint16_t* depth = calloc(DISC_SIDE * DISC_SIDE, sizeof *depth);
    void* work = malloc(contact_blobs_work_bytes());
    if (color == NULL || depth == NULL || work == NULL) {
        free(work);
        free(depth);
        free(color);
        return EXIT_FAILURE;
    }
    r3d_lens_t lens = {
        .width = DISC_SIDE,
        .height = DISC_SIDE,
        .focal_x = DISC_FOCAL,
        .focal_y = DISC_FOCAL,
        .m = {{{DISC_FOCAL, 0, 0, 0}, {0, 0, -DISC_FOCAL, 0}, {0, 1, 0, DISC_DEPTH}, {0, 0, 0, 1}}},
        .project = {.cx = DISC_SIDE / 2.0F, .cy = DISC_SIDE / 2.0F, .near_z = 1.0F},
        .depth_near = 1.0F,
    };
    r3d_span_target_t target = {0};
    gfx_target_of_color_depth(&target.rows, color, depth, DISC_SIDE, DISC_SIDE, 0);
    printf("{");
    print_discs(&target, &lens, work);
    print_shapes(&target, &lens, work);
    printf("}\n");
    free(work);
    free(depth);
    free(color);
    return 0;
}
