/*
 * Portable suite: ui/ui_icons, an icon set loaded from a pack written here:
 * it holds one use of the pack while loaded and none after, and a missing
 * name, an entry of another format or a missing pack loads nothing and holds
 * nothing. On a host only, since the pack is a file.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "suites.h"
#include "unity.h"

#include "asset/asset_store.h"
#include "gfx/image/gfx_image.h"
#include "test_pack.h"
#include "ui/ui_icons.h"

#ifndef DEVICE_BUILD
#include "test_asset_dir.h"

#define PACK        "suite_ui_icons"
#define PACK_BYTES  512
#define IMAGE_BYTES 32 /* a 16-byte header and a 1 x 1 picture */

/* One image entry of `format`, 1 x 1, its pixel after the header. */
static void
put_image(test_pack_t* pack, const char* name, gfx_image_format_t format) {
    enum { HEADER = 16 };

    uint8_t* at = test_pack_add(pack, name, GFX_IMAGE_ASSET, IMAGE_BYTES);
    test_pack_put16(at, GFX_IMAGE_VERSION);
    test_pack_put16(at + 2, format);
    test_pack_put16(at + 4, 1);
    test_pack_put16(at + 6, 1);
    test_pack_put32(at + 8, format == GFX_IMAGE_MONO1 ? GFX_IMAGE_MONO1_STRIDE_STEP : 1U);
    test_pack_put32(at + 12, HEADER);
    at[HEADER] = 0x80;
}

/* Pack PACK: "mark", one bit a pixel, and "photo", RGB565. */
static void
write_icon_pack(void) {
    uint8_t* bytes = malloc(PACK_BYTES);
    TEST_ASSERT_NOT_NULL(bytes);
    test_pack_t pack = test_pack_begin(bytes, PACK_BYTES, 2);
    put_image(&pack, "mark", GFX_IMAGE_MONO1);
    put_image(&pack, "photo", GFX_IMAGE_RGB565);
    const uint32_t size = test_pack_finish(&pack);
    test_write_file("./" PACK ".apak", bytes, size);
    free(bytes);
}

/* Loads a set of one icon named `name` from `pack`; says whether it loaded,
 * and checks that only a loaded set holds a store slot, and none after
 * it is released. */
static bool
load_one(const char* pack, const char* name) {
    const ui_icon_name_t names[] = {{name, "m"}};
    gfx_image_t icons[1];
    ui_icon_set_t set = {pack, names, 1, icons, false};
    const int mounted = asset_store_mounted();
    const bool loaded = ui_icon_set_load(&set);
    TEST_ASSERT_EQUAL_INT(loaded, set.loaded);
    TEST_ASSERT_EQUAL_INT(mounted + (loaded ? 1 : 0), asset_store_mounted());
    TEST_ASSERT_EQUAL_INT(loaded, ui_icon_set_get(&set, 0) != NULL);
    TEST_ASSERT_TRUE_MESSAGE(ui_icon_set_load(&set) == loaded, "a second load changed the answer");
    TEST_ASSERT_EQUAL_INT(mounted + (loaded ? 1 : 0), asset_store_mounted());
    ui_icon_set_release(&set);
    TEST_ASSERT_EQUAL_INT(mounted, asset_store_mounted());
    TEST_ASSERT_NULL(ui_icon_set_get(&set, 0));
    return loaded;
}

static void
test_a_set_of_one_bit_icons_loads_and_holds_its_pack_until_released(void) {
    test_asset_dir_use(".");
    write_icon_pack();
    TEST_ASSERT_TRUE(load_one(PACK, "mark"));
    (void)remove("./" PACK ".apak");
    test_asset_dir_restore();
}

static void
test_a_missing_name_an_image_of_another_format_or_no_pack_loads_nothing(void) {
    test_asset_dir_use(".");
    write_icon_pack();
    TEST_ASSERT_FALSE(load_one(PACK, "nope"));
    TEST_ASSERT_FALSE(load_one(PACK, "photo"));
    TEST_ASSERT_FALSE(load_one("suite_ui_icons_missing", "mark"));
    (void)remove("./" PACK ".apak");
    test_asset_dir_restore();
}
#endif

void
suite_ui_icons(void) {
#ifndef DEVICE_BUILD
    RUN_TEST(test_a_set_of_one_bit_icons_loads_and_holds_its_pack_until_released);
    RUN_TEST(test_a_missing_name_an_image_of_another_format_or_no_pack_loads_nothing);
#endif
}

SUITE_REGISTER(suite_ui_icons);
