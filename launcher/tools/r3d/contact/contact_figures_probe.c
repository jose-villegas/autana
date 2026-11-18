/* The numbers behind contact_figures.py: contact_blob_draw()'s discs at several strengths, seen straight down.
 * Prints JSON: each strength's mask of halved pixels. */

#include <stdio.h>
#include <stdlib.h>

#include "render/contact/contact_blob.h"
#include "scene/scene.h"

/* The contact-disc pictures: a camera looking straight down a ground plane at depth DISC_DEPTH. */
#define DISC_SIDE   72
#define DISC_FOCAL  72.0F
#define DISC_DEPTH  4.0F
#define DISC_RADIUS 1.75F
static const float DISC_STRENGTHS[] = {0.25F, 0.5F, SCENE_CONTACT_BLOB_STRENGTH_DEFAULT, 1.0F};
#define DISC_COUNT ((int)(sizeof DISC_STRENGTHS / sizeof DISC_STRENGTHS[0]))
#define WHITE      0xffffU

static void
print_discs(void) {
    uint16_t* color = malloc(DISC_SIDE * DISC_SIDE * sizeof *color);
    uint16_t* depth = calloc(DISC_SIDE * DISC_SIDE, sizeof *depth);
    if (color == NULL || depth == NULL) {
        exit(1);
    }
    r3d_lens_t lens = {.width = DISC_SIDE, .height = DISC_SIDE, .focal_x = DISC_FOCAL, .focal_y = DISC_FOCAL};
    lens.m = mat4f_identity();
    lens.m.m[0][0] = DISC_FOCAL;
    lens.m.m[1][1] = -DISC_FOCAL;
    lens.project = (projectf_t){.cx = DISC_SIDE / 2.0F, .cy = DISC_SIDE / 2.0F, .near_z = 1.0F};
    lens.depth_near = 1.0F;
    printf("\"disc\": {\"radius_px\": %g, \"side\": %d, \"strengths\": [", DISC_RADIUS * DISC_FOCAL / DISC_DEPTH,
           DISC_SIDE);
    for (int s = 0; s < DISC_COUNT; s++) {
        printf("%s%g", s ? ", " : "", DISC_STRENGTHS[s]);
    }
    printf("], \"default\": %g, \"covered\": [", SCENE_CONTACT_BLOB_STRENGTH_DEFAULT);
    for (int s = 0; s < DISC_COUNT; s++) {
        for (int i = 0; i < DISC_SIDE * DISC_SIDE; i++) {
            color[i] = WHITE;
        }
        r3d_span_target_t target = {0};
        gfx_target_of_color_depth(&target.rows, color, depth, DISC_SIDE, DISC_SIDE, 0);
        contact_blob_draw(&target, &lens, (vec3f_t){0.0F, 0.0F, DISC_DEPTH}, vec3f_unit_z(), DISC_RADIUS,
                          DISC_STRENGTHS[s]);
        printf("%s[", s ? ", " : "");
        for (int i = 0; i < DISC_SIDE * DISC_SIDE; i++) {
            printf("%s%d", i ? "," : "", color[i] != WHITE);
        }
        printf("]");
    }
    printf("]}");
    free(depth);
    free(color);
}

int
main(void) {
    printf("{");
    print_discs();
    printf("}\n");
    return 0;
}
