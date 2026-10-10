#define _POSIX_C_SOURCE 200809L /* strdup */

#include "pack_reads.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "suites.h"

#define WAITING_ENV "AUTANA_PACKS_WAITING"

const asset_pack_t* __real_asset_store_pack(const char* name);
const asset_pack_t* __wrap_asset_store_pack(const char* name);

static char* runner_dir;
static char* waiting; /* WAITING_ENV's value, cut at its commas; suites.c keeps pointers into it */
static int undeclared;

void
pack_reads_begin(void) {
    const char* dir = getenv(ASSET_STORE_DIR_ENV);
    runner_dir = dir == NULL ? NULL : strdup(dir);
    const char* list = getenv(WAITING_ENV);
    if (list == NULL || list[0] == '\0') {
        return;
    }
    waiting = strdup(list);
    for (char* pack = waiting; pack != NULL;) {
        char* comma = strchr(pack, ',');
        if (comma != NULL) {
            *comma = '\0';
        }
        if (pack[0] != '\0') {
            suites_wait_on(pack);
        }
        pack = comma != NULL ? comma + 1 : NULL;
    }
}

int
pack_reads_undeclared(void) {
    return undeclared;
}

const asset_pack_t*
__wrap_asset_store_pack(const char* name) {
    const char* dir = getenv(ASSET_STORE_DIR_ENV);
    if (runner_dir != NULL && dir != NULL && strcmp(dir, runner_dir) == 0 && !suites_current_reads(name)) {
        const char* suite = suites_current();
        undeclared++;
        printf("FAIL: suite %s read pack %s without naming it: add SUITE_READS(%s, ...) beside its SUITE_REGISTER\n",
               suite == NULL ? "(none)" : suite, name, suite == NULL ? "suite" : suite);
    }
    return __real_asset_store_pack(name);
}
