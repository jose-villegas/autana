/* display_quarter_now for a render_host program, whose scene shell asks it:
 * the run's quarter. A strong definition, since some MinGW linkers leave a
 * call to a weak one unresolved. */
#include "display/display.h"
#include "render_host.h"

int
display_quarter_now(void) {
    return render_host_frame()->quarter;
}
