/* Owner snapshots and the changes a screen requests. No values are retained. */
#pragma once
#include <stdbool.h>

typedef struct {
    bool fast_clock;
    bool overlay_on;
    bool leaf_on;
    bool interlace_on;
    bool send_audit_on;
} settings_snapshot_t;

typedef struct {
    settings_snapshot_t values;
} settings_result_t;

typedef struct {
    bool panel_clock;
    bool overlay;
    bool leaf;
    bool interlace;
    bool send_audit;
} settings_changes_t;

settings_changes_t settings_ui_changes(const settings_snapshot_t* snapshot, const settings_result_t* result);
