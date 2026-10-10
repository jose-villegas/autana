/* settings_policy: settings_policy.h's rule. */

#include "services/settings_policy.h"

int
settings_store_start(const settings_store_ops_t* ops) {
    int result = ops->init();
    if (result == ops->stale_full || result == ops->stale_format) {
        result = ops->erase();
        if (result == 0) {
            result = ops->init();
        }
    }
    return result;
}
