#include "apps/render_lab/tests/render_lab_test.h"
#ifdef DEVICE_BUILD
#include <stdio.h>
#include "apps/render_lab/render_lab_dynres.h"

const char*
render_lab_test_step_name(int step) {
    static char name[sizeof "-2147483648x-2147483648"];
    const resolution_step_t size = render_lab_ladder[step];
    (void)snprintf(name, sizeof name, "%dx%d", size.width, size.height);
    return name;
}
#endif
