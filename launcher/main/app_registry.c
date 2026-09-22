/*
 * app_registry - the one app_t list, threaded through app_t.next.
 *
 * Apps register themselves, so an app is entirely contained in
 * main/apps/<name>/ and deleting that folder removes it - source, logic and
 * tests - without touching another file, CMakeLists.txt included.
 *
 * Portable: no hardware dependency, so the firmware, the host test runner,
 * the editor's runtime and every render-host harness link this one list
 * and call app_register() on whatever app_t they have.
 */
#include "app.h"

#include <assert.h>
#include <string.h>

static app_t* apps_head;

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
