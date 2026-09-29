#include "util/build_id.h"

#include "build_variant.h"

#include "esp_app_desc.h"

#include <string.h>

#if CONFIG_LAUNCHER_SELFTEST
#define BUILD_ID_VARIANT "diag"
#elif CONFIG_LAUNCHER_DEVELOPMENT
#define BUILD_ID_VARIANT "dev"
#else
#define BUILD_ID_VARIANT "release"
#endif

static char id[BUILD_ID_LINE_MAX];
static char short_id[BUILD_ID_SHORT_CHARS + 1];

const char*
build_id(void) {
    esp_app_get_elf_sha256(id, BUILD_ID_HASH_CHARS + 1);
    const int variant_length =
        snprintf(id + BUILD_ID_HASH_CHARS, sizeof(id) - BUILD_ID_HASH_CHARS, "-%s", BUILD_ID_VARIANT);
    if (variant_length < 0 || (size_t)variant_length >= sizeof(id) - BUILD_ID_HASH_CHARS) {
        id[0] = '\0';
    }
    return id;
}

const char*
build_id_short(void) {
    memcpy(short_id, build_id(), BUILD_ID_SHORT_CHARS);
    short_id[BUILD_ID_SHORT_CHARS] = '\0';
    return short_id;
}
