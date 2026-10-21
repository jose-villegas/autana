/* The numbers behind skin_light_figures.py: one white light lighting a normal swept from facing it to facing
 * away, through the runtime's direct sum and its irradiance table. Prints JSON. */

#include <math.h>
#include <stdio.h>

#include "render/skin/r3d_skin_light.h"
#include "render/skin/r3d_skin_light_internal.h"

/* The sweep: SAMPLES normals from the light's direction to its opposite, turned AZIMUTH_DEGREES about it so
 * the path crosses the table's cells rather than running along one row. */
#define SAMPLES         361
#define AZIMUTH_DEGREES 30.0F
#define DEGREES_PER_PI  180.0F
/* A blended normal this long, as joints mixing leave it, beside a unit one. */
#define SHORT_LENGTH    0.8F

static r3d_skin_light_table_t table;

int
main(void) {
    r3d_skin_lights_t lights = {.count = 1, .direction = {{0.0F, 0.0F, 1.0F}}, .color = {{1.0F, 1.0F, 1.0F}}};
    lights.table_scale = r3d_skin_light_table_build(&lights, &table);
    lights.table = &table;
    const float azimuth = AZIMUTH_DEGREES * (float)M_PI / DEGREES_PER_PI;
    printf("{\"short_length\": %g, \"table_side\": %d, \"samples\": [", SHORT_LENGTH, R3D_SKIN_LIGHT_TABLE_SIDE);
    for (int i = 0; i < SAMPLES; i++) {
        const float angle = (float)M_PI * (float)i / (float)(SAMPLES - 1);
        const vec3f_t unit = {sinf(angle) * cosf(azimuth), sinf(angle) * sinf(azimuth), cosf(angle)};
        const float exact = light_irradiance(unit, &lights).x;
        const float short_normal = light_irradiance(vec3f_scale(unit, SHORT_LENGTH), &lights).x;
        const float sampled = r3d_skin_light_table_sample(unit, &lights).x;
        printf("%s[%g, %g, %g, %g]", i ? ", " : "", angle * DEGREES_PER_PI / (float)M_PI, exact, short_normal, sampled);
    }
    printf("]}\n");
    return 0;
}
