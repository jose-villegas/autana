/*
 * test_asset_dir: host only. Points the store's file backend at a folder for
 * one test and back at the runner's, and writes the pack files a test makes.
 */
#pragma once

#include <stddef.h>

/* Sets AUTANA_ASSET_DIR to `dir`, keeping the runner's value for
 * test_asset_dir_restore(). */
void test_asset_dir_use(const char* dir);

void test_asset_dir_restore(void);

/* Writes `size` bytes to `path`, failing the test when it cannot. */
void test_write_file(const char* path, const void* bytes, size_t size);
