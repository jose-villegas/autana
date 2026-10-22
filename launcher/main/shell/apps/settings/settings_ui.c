#include "shell/apps/settings/settings_ui.h"

settings_changes_t
settings_ui_changes(const settings_snapshot_t* snapshot, const settings_result_t* result) {
    const settings_changes_t changes = {
        .panel_clock = snapshot->fast_clock != result->values.fast_clock,
        .overlay = snapshot->overlay_on != result->values.overlay_on,
        .leaf = snapshot->leaf_on != result->values.leaf_on,
        .interlace = snapshot->interlace_on != result->values.interlace_on,
        .send_audit = snapshot->send_audit_on != result->values.send_audit_on,
    };
    return changes;
}
