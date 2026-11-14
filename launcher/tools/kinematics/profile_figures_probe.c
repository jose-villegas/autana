/* profile_figures_probe: a full move and a short one through motion_profile_at() for the profile reference. */
#include <stdio.h>

#include "kinematics/profile/motion_profile.h"

static const float step_s = 0.02F;
static const motion_profile_t profiles[] = {
    {.start = 0.0F, .end = 10.0F, .peak_speed = 2.0F, .rest_s = 1.0F, .ramp_s = 2.0F},
    {.start = 0.0F, .end = 2.0F, .peak_speed = 2.0F, .rest_s = 1.0F, .ramp_s = 2.0F},
};

int
main(void) {
    printf("{\"profiles\":[");
    for (size_t p = 0; p < sizeof profiles / sizeof profiles[0]; p++) {
        const motion_profile_t* profile = &profiles[p];
        const float duration = motion_profile_duration_s(profile);
        printf("%s{\"distance\":%.9g,\"cruise\":%.9g,\"duration\":%.9g,\"samples\":[", p ? "," : "",
               (double)(profile->end - profile->start), (double)motion_profile_cruise_speed(profile), (double)duration);
        for (int i = 0; (float)i * step_s <= duration; i++) {
            const motion_profile_at_t at = motion_profile_at(profile, (float)i * step_s);
            printf("%s[%.9g,%.9g,%.9g]", i ? "," : "", (double)((float)i * step_s), (double)at.at, (double)at.speed);
        }
        printf("]}");
    }
    puts("]}");
    return 0;
}
