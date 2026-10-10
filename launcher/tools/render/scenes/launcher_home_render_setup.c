/* Shared launcher registry and settled backdrop for host scenes. */
#include "launcher_home_render_setup.h"

#include <stdio.h>

#include "app/app.h"
#include "gfx/gfx.h"
#include "ui/ui.h"
#include "ui/ui_launcher.h"
#include "ui/ui_ridge.h"
#include "ui/ui_transform.h"

static app_t fixture_alpha = {.name = "Alpha", .summary = "The first fixture row"};
static app_t fixture_beta = {.name = "Beta", .summary = "The second fixture row"};
static app_t fixture_gamma = {.name = "Gamma", .summary = "The third fixture row"};

#define ROWS_MAX 16

static app_t stated[ROWS_MAX];
static int stated_count;

static void
register_fixture(void) {
    app_register(&fixture_alpha);
    app_register(&fixture_beta);
    app_register(&fixture_gamma);
}

bool
launcher_home_render_add_row(const char* name) {
    if (stated_count >= ROWS_MAX) {
        fprintf(stderr, "at most %d rows\n", ROWS_MAX);
        return false;
    }
    stated[stated_count++].name = name;
    return true;
}

void
launcher_home_render_register_rows(void) {
    if (stated_count > 0) {
        for (int i = 0; i < stated_count; i++) {
            app_register(&stated[i]);
        }
    } else {
        register_fixture();
    }
}

void
launcher_home_render_setup(int quarter) {
    static const int down[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
    ui_launcher_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    ui_ridge_set_gravity(down[quarter & 3][0], down[quarter & 3][1], 256, 0);
    ui_ridge_set_ambient(false);
    ui_ridge_settle();
}
