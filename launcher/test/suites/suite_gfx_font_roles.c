/*
 * Portable suite for the UI font role's fixed font identity.
 * gfx.h requires the BSP; gfx_font_roles.h compiles on the host.
 */

#include "suites.h"
#include "unity.h"

#include "gfx/gfx_font_roles.h"

/* gfx_font_ui() - the UI/body-text role */

/* Pointer identity, not a field-by-field comparison: gfx_font_ui() promises
 * to hand back gfx_font_8x8 itself, the one instance every drawing path
 * already reads its atlas/metrics from, not a copy that merely looks the
 * same today. A copy would still pass a field comparison while breaking the
 * whole point of a compile-time role - see gfx_font_roles.h's own "static
 * inline" reasoning. */
static void
test_ui_role_is_the_shipped_8x8_font(void) {
    TEST_ASSERT_EQUAL_PTR_MESSAGE(&gfx_font_8x8, gfx_font_ui(),
                                  "gfx_font_ui() must resolve to gfx_font_8x8 itself - every UI call "
                                  "site this role replaced (gfx.c, ui.c, boot_anim.c) drew with that "
                                  "exact font before the role existed, and the migration promised no "
                                  "visual change");
}

/* gfx_font_ui() takes no arguments, so calling it twice must be provably
 * idempotent - the only way a `static inline` accessor like this could ever
 * disagree with itself is a stray global it should not have. */
static void
test_ui_role_is_stable_across_calls(void) {
    TEST_ASSERT_EQUAL_PTR(gfx_font_ui(), gfx_font_ui());
}

void
run_gfx_font_roles_suite(void) {
    RUN_TEST(test_ui_role_is_the_shipped_8x8_font);
    RUN_TEST(test_ui_role_is_stable_across_calls);
}

SUITE_REGISTER(run_gfx_font_roles_suite);
