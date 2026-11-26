/* clock_fake: deterministic host wall clock shared by tests and render tools. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

void clock_fake_set(int64_t now_us);
/* Reads the caller-owned timestamps in order, then holds the last one. */
void clock_fake_script(const int64_t* readings_us, size_t count);
void clock_fake_reset(void);
bool clock_fake_read(struct timespec* now);
