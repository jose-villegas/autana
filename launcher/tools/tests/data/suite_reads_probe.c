/*
 * suite_reads_probe: the host runner's SUITE_READS check and skip
 * (test/suites.c, test/pack_reads.c) around fake suites, linked with
 * --wrap=asset_store_pack as test/run_tests.sh links host_tests. Each suite
 * prints "RAN <suite>"; -DREAD_UNDECLARED adds one that reads a pack it does
 * not name. The store is suite_reads_store.c: --wrap reaches only calls from
 * another object. test_suite_reads.py builds and runs it.
 */
#define _POSIX_C_SOURCE 200809L /* setenv */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "asset/asset_store.h"
#include "pack_reads.h"
#include "suites.h"

#define WAITING_PACK "probe_waiting"
#define BUILT_PACK   "probe_built"
#define OWN_PACK     "probe_own"
#define OWN_DIR      "probe_own_folder"
#define RUNNER_DIR   "probe_runner_folder"

static void
read_and_say(const char* suite, const char* pack) {
    (void)asset_store_pack(pack);
    printf("RAN %s\n", suite);
}

static void
run_reads_waiting(void) {
    read_and_say("run_reads_waiting", WAITING_PACK);
}

SUITE_REGISTER(run_reads_waiting);
SUITE_READS(run_reads_waiting, WAITING_PACK);

static void
run_reads_built(void) {
    read_and_say("run_reads_built", BUILT_PACK);
}

SUITE_REGISTER(run_reads_built);
SUITE_READS(run_reads_built, BUILT_PACK);

static void
run_reads_nothing(void) {
    printf("RAN run_reads_nothing\n");
}

SUITE_REGISTER(run_reads_nothing);

static void
set_dir(const char* dir) {
#ifdef _WIN32
    (void)_putenv_s(ASSET_STORE_DIR_ENV, dir);
#else
    (void)setenv(ASSET_STORE_DIR_ENV, dir, 1);
#endif
}

/* A suite that points the folder at its own packs is not checked. */
static void
run_reads_its_own_folder(void) {
    const char* runner = getenv(ASSET_STORE_DIR_ENV);
    char* saved = runner == NULL ? NULL : strdup(runner);
    set_dir(OWN_DIR);
    read_and_say("run_reads_its_own_folder", OWN_PACK);
    if (saved != NULL) {
        set_dir(saved);
    }
    free(saved);
}

SUITE_REGISTER(run_reads_its_own_folder);

#ifdef READ_UNDECLARED
static void
run_reads_undeclared(void) {
    read_and_say("run_reads_undeclared", BUILT_PACK);
}

SUITE_REGISTER(run_reads_undeclared);
#endif

int
main(void) {
    set_dir(RUNNER_DIR); /* the folder run_tests.sh would name */
    pack_reads_begin();
    suites_run_all();
    suites_print_waiting();
    return pack_reads_undeclared() + suites_dropped();
}
