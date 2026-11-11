/* app_registry: one app_t.next list, no hardware dependency; firmware, host test
 * runner and every render-host harness share it. Apps self-register:
 * adding or deleting main/apps/<name>/ (source, logic, tests) touches no other file, including CMakeLists.txt. */
#include "app/app.h"

#include <assert.h>
#include <string.h>

#include "core/registry.h"

static app_t* apps_head;

/* Home order: each kind's rank, then the name. */
static int
kind_rank(app_kind_t kind) {
    static const int RANK[] = {[APP_KIND_LAB] = 0, [APP_KIND_APP] = 1, [APP_KIND_SYSTEM] = 2};
    return RANK[kind];
}

static int
home_order(const void* a, const void* b) {
    const app_t* x = a;
    const app_t* y = b;
    const int by_kind = kind_rank(x->kind) - kind_rank(y->kind);
    return by_kind != 0 ? by_kind : strcmp(x->name, y->name);
}

void
app_register(app_t* app) {
    static const registry_rule_t rule = REGISTRY_RULE(app_t, strcmp, home_order);
    const registry_add_t result = registry_add(&apps_head, &rule, app);
    assert(result == REGISTRY_ADDED && "app registered twice or name taken");
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
