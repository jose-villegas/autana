/*
 * app_registry - the one app_t list, threaded through app_t.next.
 *
 * Apps register themselves, so an app is entirely contained in
 * main/apps/<name>/ and deleting that folder removes it - source, logic and
 * tests - without touching another file, CMakeLists.txt included.
 *
 * Portable: this file has no hardware dependency of its own, so it is
 * compiled into the firmware, the host test runner, the editor's runtime
 * and every render-host harness alike - each of those used to hand-copy
 * its own array-backed registry (or a fake standing in for one); now they
 * all link this and call app_register() on whatever app_t they have.
 */
#include "app.h"

#include <assert.h>
#include <string.h>

static app_t* apps_head;
static int apps_registered;

void
app_register(app_t* app) {
    app->next = NULL;

    app_t** link = &apps_head;
    while (*link != NULL && strcmp((*link)->name, app->name) < 0) {
        link = &(*link)->next;
    }
    assert((*link == NULL || strcmp((*link)->name, app->name) != 0) && "two apps share a name");
    app->next = *link;
    *link = app;
    apps_registered++;
}

const app_t*
app_list(void) {
    return apps_head;
}

int
app_registry_count(void) {
    return apps_registered;
}

#ifndef ESP_PLATFORM
/* Host-only: nothing device-side ever needs to un-register an app - a real
 * boot registers each once and the image runs until it reboots - but a
 * host test process runs many scenarios in one run, each wanting its own
 * clean list. Absent from every device build. */
void
app_registry_reset_for_test(void) {
    apps_head = NULL;
    apps_registered = 0;
}
#endif
