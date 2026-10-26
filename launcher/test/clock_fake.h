/* clock_fake: deterministic host wall clock shared by tests and render tools. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

void clock_fake_set(int64_t now_us);
void clock_fake_reset(void);
bool clock_fake_read(struct timespec* now);
