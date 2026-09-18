/*
 * build_variant - CONFIG_LAUNCHER_{RELEASE,DEVELOPMENT,SELFTEST}, reached on
 * purpose rather than by whichever ESP-IDF header happened to already be
 * included.
 *
 * ESP-IDF does not force sdkconfig.h into every translation unit - a file
 * only sees a CONFIG_* macro if it, or something it includes, asks for
 * sdkconfig.h first. Every CONFIG_LAUNCHER_* test in this tree used to rely
 * on that by accident (usually esp_log.h dragging sdkconfig.h in first),
 * which fails silently the moment a file's own includes are all portable
 * ones: a whole `#if CONFIG_LAUNCHER_DEVELOPMENT` block compiles out, with
 * no error, on every build variant including the ones meant to carry it.
 * Any file testing CONFIG_LAUNCHER_* includes this instead of leaving that
 * to chance.
 *
 * A host build defines no ESP_PLATFORM and has no sdkconfig.h to find, so
 * every CONFIG_LAUNCHER_* macro is simply undefined there - #if treats that
 * as 0, the existing, deliberate way a host test opts out of a
 * variant-gated branch.
 */
#pragma once

#ifdef ESP_PLATFORM
#include "sdkconfig.h"

#if !defined(CONFIG_LAUNCHER_RELEASE) && !defined(CONFIG_LAUNCHER_DEVELOPMENT) && !defined(CONFIG_LAUNCHER_SELFTEST)
#error                                                                                                                 \
    "sdkconfig.h did not define the LAUNCHER_BUILD_TYPE choice - the include above is broken, not the variant choice itself."
#endif

#endif
