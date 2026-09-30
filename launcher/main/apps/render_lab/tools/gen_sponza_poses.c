/*
 * Prints the Sponza flythrough as a poses file for tools/r3d's
 * triangle_sizes: the lens and render size scene_sponza.c uses, and the
 * poses sponza_poses() gives, the ones suite_sponza_perf.c times.
 */
#include <stdio.h>

#include "apps/render_lab/sponza_flythrough.h"

#define POSES_MAX 64

int
main(void) {
    r3d_vec3f_t eye[POSES_MAX];
    r3d_vec3f_t forward[POSES_MAX];
    const int count = sponza_poses(eye, forward, POSES_MAX);
    int failed = printf("size %d %d\nlens %.6g %.6g\n", SPONZA_RENDER_WIDTH, SPONZA_RENDER_HEIGHT,
                        (double)SPONZA_HALF_FOV_SHORT_TAN, (double)SPONZA_NEAR_Z)
                 < 0;
    for (int i = 0; i < count; i++) {
        failed |= printf("pose %.9g %.9g %.9g %.9g %.9g %.9g\n", (double)eye[i].x, (double)eye[i].y, (double)eye[i].z,
                         (double)forward[i].x, (double)forward[i].y, (double)forward[i].z)
                  < 0;
    }
    return failed;
}
