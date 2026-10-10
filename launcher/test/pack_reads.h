/*
 * pack_reads: the host runner's check on SUITE_READS (suites.h). Every
 * asset_store_pack() call reaches __wrap_asset_store_pack (the link wraps it,
 * as it wraps malloc); one that reads the runner's asset folder for a pack the
 * running suite did not name is counted, and the runner fails on the count.
 * A suite that points AUTANA_ASSET_DIR at a folder of its own
 * (test_asset_dir.h) reads its own packs and is not checked.
 */
#pragma once

/* Takes the runner's AUTANA_ASSET_DIR as the folder to check, and each pack
 * AUTANA_PACKS_WAITING (comma-separated) names as one that was not built
 * (suites_wait_on()). Call once, before any suite runs. */
void pack_reads_begin(void);

/* How many reads named no SUITE_READS so far. */
int pack_reads_undeclared(void);
