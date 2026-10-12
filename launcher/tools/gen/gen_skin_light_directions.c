/* Flash directions share the runtime octahedral decode. */
#include <math.h>
#include <stdio.h>
#include "math/linear/vec3f.h"
#include "render/skin/r3d_skin_light.h"

int
main(void) {
    vec3f_t directions[R3D_SKIN_LIGHT_TABLE_CELLS];
    const float tolerance = 0.00001F;
    for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
        const vec2f_t point = {2.0F * (cell % R3D_SKIN_LIGHT_TABLE_SIDE) / (R3D_SKIN_LIGHT_TABLE_SIDE - 1) - 1.0F,
                               2.0F * (cell / R3D_SKIN_LIGHT_TABLE_SIDE) / (R3D_SKIN_LIGHT_TABLE_SIDE - 1) - 1.0F};
        const vec3f_t direction = vec3f_from_octahedral(point);
        directions[cell] = direction;
        if (!isfinite(direction.x) || !isfinite(direction.y) || !isfinite(direction.z)
            || fabsf(vec3f_dot(direction, direction) - 1.0F) > tolerance) {
            return 1;
        }
    }
    puts("/* GENERATED FILE - do not edit.\n"
         " * sh tools/gen/gen_skin_light_directions.sh > main/render/skin/r3d_skin_light_directions.h\n"
         " */\n#pragma once\n#include \"render/skin/r3d_skin_light.h\"");
    printf("_Static_assert(R3D_SKIN_LIGHT_TABLE_SIDE == %d, \"regenerate irradiance directions\");\n",
           R3D_SKIN_LIGHT_TABLE_SIDE);
    puts("static const vec3f_t light_table_directions[R3D_SKIN_LIGHT_TABLE_CELLS] = {");
    for (int cell = 0; cell < R3D_SKIN_LIGHT_TABLE_CELLS; cell++) {
        const vec3f_t direction = directions[cell];
        printf("    {%.9eF, %.9eF, %.9eF},\n", (double)direction.x, (double)direction.y, (double)direction.z);
    }
    puts("};");
    return ferror(stdout) ? 1 : 0;
}
