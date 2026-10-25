/* asset_store_shell: core-0 progress and app-exit cleanup for the frame loop. */
#pragma once
#include <stdint.h>
void asset_store_step(uint32_t dt_ms);
void asset_store_app_exit(void);
