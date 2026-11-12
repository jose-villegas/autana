/* Shared launcher registry and settled backdrop for host scenes. */
#include "launcher_home_render_setup.h"

#include <stdio.h>
#include <string.h>

#include "app/app.h"
#include "gfx/gfx.h"
#include "shell/ui/ui_launcher.h"
#include "shell/ui/ui_ridge.h"
#include "ui/layout/ui_transform.h"
#include "ui/ui.h"

static app_t fixture_alpha = {.name = "Alpha", .summary = "The first fixture row"};
static app_t fixture_beta = {.name = "Beta", .summary = "The second fixture row"};
static app_t fixture_gamma = {.name = "Gamma", .summary = "The third fixture row"};

#define ROWS_MAX       16

/* A stated row's name, its ":kind" cut off. */
#define NAME_MAX_BYTES 64

static app_t stated[ROWS_MAX];
static char names[ROWS_MAX][NAME_MAX_BYTES];
static int stated_count;

static void
register_fixture(void) {
    app_register(&fixture_alpha);
    app_register(&fixture_beta);
    app_register(&fixture_gamma);
}

/* What "Name:kind" may say after the colon; with no colon it is an app. */
static const struct {
    const char* word;
    app_kind_t kind;
} KINDS[] = {{"lab", APP_KIND_LAB}, {"system", APP_KIND_SYSTEM}};

bool
launcher_home_render_add_row(const char* stated_row) {
    if (stated_count >= ROWS_MAX) {
        fprintf(stderr, "at most %d rows\n", ROWS_MAX);
        return false;
    }
    char* name = names[stated_count];
    if (snprintf(name, NAME_MAX_BYTES, "%s", stated_row) >= NAME_MAX_BYTES) {
        fprintf(stderr, "row name longer than %d bytes: %s\n", NAME_MAX_BYTES - 1, stated_row);
        return false;
    }
    app_t* row = &stated[stated_count];
    *row = (app_t){.name = name};
    char* word = strrchr(name, ':');
    if (word != NULL) {
        *word++ = '\0';
        size_t k = 0;
        while (k < sizeof KINDS / sizeof KINDS[0] && strcmp(KINDS[k].word, word) != 0) {
            k++;
        }
        if (k == sizeof KINDS / sizeof KINDS[0]) {
            fprintf(stderr, "unknown kind '%s' in row '%s': lab or system\n", word, stated_row);
            return false;
        }
        row->kind = KINDS[k].kind;
    }
    stated_count++;
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
    ui_ridge_set_gravity((vec3f_t){down[quarter & 3][0], down[quarter & 3][1], 0}, 0);
    ui_ridge_set_ambient(false);
    ui_ridge_settle();
}
