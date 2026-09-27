/* Portable suite: the launcher backdrop's seed-derived palette. */

#include "suites.h"
#include "unity.h"

#include "ui/ridge_theme.h"

static void
test_seed_keeps_its_hue_and_roles_stay_in_gamut(void) {
    const ridge_theme_t theme = ridge_theme_from_rgb(0x1199C8);
    const ridge_oklch_t seed = ridge_theme_to_oklch(theme.seed_rgb);
    const uint32_t rgb[] = {theme.background_rgb, theme.sky_top_rgb, theme.sky_bottom_rgb, theme.back0_rgb,
                            theme.back1_rgb};
    const float lightness[] = {0.55f, 0.55f, 0.64f, 0.49f, 0.42f};

    TEST_ASSERT_EQUAL_UINT32(0x1199C8, theme.seed_rgb);
    for (int role = 0; role < 5; role++) {
        const ridge_oklch_t got = ridge_theme_to_oklch(rgb[role]);
        TEST_ASSERT_FLOAT_WITHIN(0.02f, lightness[role], got.l);
        TEST_ASSERT_FLOAT_WITHIN(0.08f, seed.h, got.h);
        TEST_ASSERT_TRUE(ridge_theme_in_gamut(got.l, got.c, got.h));
        TEST_ASSERT_EQUAL_HEX16(GFX_RGB(rgb[role]), GFX_RGB(gfx_color_rgb888(GFX_RGB(rgb[role]))));
    }
}

void
suite_ridge_theme(void) {
    RUN_TEST(test_seed_keeps_its_hue_and_roles_stay_in_gamut);
}

SUITE_REGISTER(suite_ridge_theme);
