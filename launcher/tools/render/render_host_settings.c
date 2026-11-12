/* The renderer's half of --set render.FIELD=value (render_host.c): the
 * values reach the main render context as an app's pack sets them. */
#include "render_host.h"

bool
render_host_apply_settings(const render_settings_t* settings) {
    render_context_set_settings(render_context_main(), settings);
    return true;
}
