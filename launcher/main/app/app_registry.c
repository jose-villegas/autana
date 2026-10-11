/*
 * app_registry: the one app_t list, threaded through app_t.next.
 *
 * Apps register themselves, so an app is entirely contained in
 * main/apps/<name>/ and deleting that folder removes it: source, logic and
 * tests, without touching another file, CMakeLists.txt included.
 *
 * Portable: no hardware dependency, so the firmware, the host test runner,
 * the editor's runtime and every render-host harness link this one list
 * and call app_register() on whatever app_t they have.
 */
#include "app/app.h"

#include <assert.h>
#include <string.h>

#include "core/registry.h"

static app_t* apps_head;

static int
entry_order(const void* a, const void* b) {
    const app_t* first = a;
    const app_t* second = b;
    return strcmp(first->name, second->name);
}

void
app_register(app_t* app) {
    static const registry_rule_t rule = {offsetof(app_t, next), entry_order, NULL};
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
