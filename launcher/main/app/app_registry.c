/* Apps self-register so deleting their folder removes them from every runtime. */
#include "app/app.h"

#include <assert.h>
#include <string.h>

#include "core/registry.h"

static app_t* apps_head;

void
app_register(app_t* app) {
    static const registry_rule_t rule = REGISTRY_RULE(app_t, strcmp, NULL);
    const registry_add_t result = registry_add(&apps_head, &rule, app);
    assert(result == REGISTRY_ADDED && "two apps share a name");
    (void)result;
}

const app_t*
app_list(void) {
    return apps_head;
}

#ifndef ESP_PLATFORM
void
app_registry_reset_for_test(void) {
    apps_head = NULL;
}
#endif
