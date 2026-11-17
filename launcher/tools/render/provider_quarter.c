/* display_quarter_now for the scene and stream providers: they draw at the
 * run's quarter. Both list this file, so a program linking both compiles it
 * once. A strong definition, since some MinGW linkers leave a call to a weak
 * one unresolved. */
#include "display/display.h"
#include "render_host.h"

int
display_quarter_now(void) {
    return render_host_frame()->quarter;
}
