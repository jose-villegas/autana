/* Paired instruction rows include identical frame setup and command-space resets. */
#ifdef DEVICE_BUILD
#include "core/timing.h"
#include "draw/draw_font_roles.h"
#include "draw/draw_image_format.h"
#include "suites.h"
#include "testkit/bench.h"
#include "ui/ui_internal.h"
#include "unity.h"

#define UI_ICOUNT_REPS        4096
#define UI_ICOUNT_ROUNDS      8
#define UI_ICOUNT_FLAGS       (MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)
#define UI_ICOUNT_LO          0
#define UI_ICOUNT_HI          100
#define UI_ICOUNT_VALUE       25
#define UI_ICOUNT_SCALE       2
#define UI_ICOUNT_ALPHA       128
#define UI_ICOUNT_RGB         0x314159
#define UI_ICOUNT_COLUMNS     2
#define UI_ICOUNT_ROWS        1
#define UI_ICOUNT_STEP        1
#define UI_ICOUNT_LABEL       "Go"
#define UI_ICOUNT_WINDOW_NAME "Bench"

static const recti_t box = {40, 100, 240, 60};
static const char* const names[] = {"One", "Two", "Three"};
static const uint8_t icon_bits[] = {0x80};
static const image_t icon = {.pixels = (void*)icon_bits,
                             .format = IMAGE_MONO1,
                             .width = 1,
                             .height = 1,
                             .pitch = DRAW_IMAGE_MONO1_STRIDE_STEP / CHAR_BIT};
static const image_t* const icons[] = {&icon, NULL, &icon};
static const char* const items[] = {"One", "Two", "Three"};
static const uint32_t swatches[] = {UI_ICOUNT_RGB, UI_ICOUNT_RGB};
static const ui_theme_t theme = {.panel_face = UI_ICOUNT_RGB,
                                 .panel_edge = UI_ICOUNT_RGB,
                                 .button_face = UI_ICOUNT_RGB,
                                 .accent = UI_ICOUNT_RGB,
                                 .accent_face = UI_ICOUNT_RGB,
                                 .on_accent = UI_ICOUNT_RGB,
                                 .text = UI_ICOUNT_RGB,
                                 .caption = UI_ICOUNT_RGB,
                                 .muted = UI_ICOUNT_RGB,
                                 .text_scale = UI_ICOUNT_SCALE};
static const ui_widget_button_t button = {.label = UI_ICOUNT_LABEL, .enabled = true};
#define UI_ICOUNT_CHOICES (sizeof names / sizeof names[0])

typedef struct {
    ui_mu_theme_t theme;
    int value, checked;
    bool on;
} ui_icount_state_t;

typedef void (*ui_icount_emit_fn)(ui_icount_state_t*);

#define UI_ICOUNT_EMIT(prefix, name, call)                                                                             \
    static void prefix##_##name(ui_icount_state_t* state) {                                                            \
        mu_Context* ctx = ui_context();                                                                                \
        const mu_Rect r = ui_mu_from_recti(box);                                                                       \
        (void)ctx;                                                                                                     \
        (void)r;                                                                                                       \
        (void)state;                                                                                                   \
        (void)(call);                                                                                                  \
    }
#define UI_ICOUNT_PAIR(name, face_call, mu_call)                                                                       \
    UI_ICOUNT_EMIT(face, name, face_call)                                                                              \
    UI_ICOUNT_EMIT(legacy, name, mu_call)

UI_ICOUNT_PAIR(screen, ui_screen(), ui_begin_screen(ctx, UI_ICOUNT_WINDOW_NAME, UI_ICOUNT_FLAGS))
UI_ICOUNT_PAIR(window, ui_window(box), mu_begin_window_ex(ctx, UI_ICOUNT_WINDOW_NAME, r, UI_ICOUNT_FLAGS))
UI_ICOUNT_EMIT(face, scroll_window, ui_scroll_window(box))
UI_ICOUNT_PAIR(row, ui_row(box.h), mu_layout_row(ctx, 1, (int[]){-1}, box.h))
UI_ICOUNT_EMIT(face, row_centre, ui_row_centre(box.w, box.h))
UI_ICOUNT_PAIR(next, ui_next(), mu_layout_next(ctx))
UI_ICOUNT_PAIR(button, ui_button(box, UI_ICOUNT_LABEL), ui_icon_button(ctx, UI_ICOUNT_LABEL, r, &button, &state->theme))
UI_ICOUNT_PAIR(tile, ui_tile(box, UI_ICOUNT_LABEL, false),
               ui_tile_button(ctx, UI_ICOUNT_LABEL, r, &button, &state->theme))
UI_ICOUNT_PAIR(toggle, ui_toggle(box, UI_ICOUNT_LABEL, &state->on),
               (mu_layout_set_next(ctx, r, 0), mu_checkbox(ctx, UI_ICOUNT_LABEL, &state->checked)))
UI_ICOUNT_PAIR(slider, ui_slider(box, UI_ICOUNT_LABEL, &state->value, UI_ICOUNT_LO, UI_ICOUNT_HI),
               ui_theme_slider_int(ctx, r, &state->value, UI_ICOUNT_LO, UI_ICOUNT_HI, UI_ICOUNT_STEP, &state->theme))
UI_ICOUNT_PAIR(dropdown, ui_dropdown(box, UI_ICOUNT_LABEL, &state->value, names, UI_ICOUNT_CHOICES),
               ui_mu_dropdown(ctx, UI_ICOUNT_LABEL, r, items, NULL, UI_ICOUNT_CHOICES, state->value, &state->theme))
UI_ICOUNT_PAIR(dropdown_icons, ui_dropdown_icons(box, UI_ICOUNT_LABEL, &state->value, names, icons, UI_ICOUNT_CHOICES),
               ui_mu_dropdown(ctx, UI_ICOUNT_LABEL, r, names, icons, UI_ICOUNT_CHOICES, state->value, &state->theme))
#define UI_ICOUNT_TEXT_PAIR(name, align, ink)                                                                          \
    UI_ICOUNT_PAIR(name, ui_##name(box, UI_ICOUNT_LABEL),                                                              \
                   ui_text_in(ctx, r, UI_ICOUNT_LABEL, state->theme.ink, UI_ICOUNT_SCALE, align))
UI_ICOUNT_TEXT_PAIR(text, UI_ALIGN_LEFT, text)
UI_ICOUNT_TEXT_PAIR(text_right, UI_ALIGN_RIGHT, text)
UI_ICOUNT_TEXT_PAIR(text_centre, UI_ALIGN_CENTRE, text)
UI_ICOUNT_TEXT_PAIR(caption, UI_ALIGN_LEFT, caption)
UI_ICOUNT_PAIR(header, ui_header(box, UI_ICOUNT_LABEL),
               ui_header_bar(ctx, r, UI_ICOUNT_LABEL, UI_ICOUNT_SCALE, &state->theme))
UI_ICOUNT_PAIR(panel, ui_panel(box), ui_mu_panel(ctx, r, &state->theme))
UI_ICOUNT_PAIR(fill, ui_fill(box, UI_ICOUNT_RGB), mu_draw_rect(ctx, r, ui_rgb(UI_ICOUNT_RGB)))
UI_ICOUNT_PAIR(outline, ui_outline(box, UI_ICOUNT_RGB), mu_draw_box(ctx, r, ui_rgb(UI_ICOUNT_RGB)))
UI_ICOUNT_PAIR(icon, ui_icon(box, &icon, NULL), ui_draw_icon(ctx, r, &icon, NULL, state->theme.text))
UI_ICOUNT_PAIR(swatches, ui_swatches(box, swatches, UI_ICOUNT_COLUMNS, UI_ICOUNT_ROWS),
               ui_swatch_grid(ctx, r, swatches, UI_ICOUNT_COLUMNS, UI_ICOUNT_ROWS))
UI_ICOUNT_EMIT(face, text_height, ui_text_height())
UI_ICOUNT_EMIT(face, theme, ui_theme(&theme))
UI_ICOUNT_EMIT(face, disabled, (ui_disabled_begin(true), ui_disabled_end()))
UI_ICOUNT_EMIT(face, color, (ui_color_begin(UI_COLOR_TEXT, UI_ICOUNT_RGB), ui_color_end()))
UI_ICOUNT_EMIT(face, scale, (ui_scale_begin(UI_ICOUNT_SCALE), ui_scale_end()))
UI_ICOUNT_EMIT(face, alpha, (ui_alpha_begin(UI_ICOUNT_ALPHA), ui_alpha_end()))

static void
measure_row(const char* name, ui_icount_emit_fn emit, bool window) {
    ui_init();
#define UI_ICOUNT_THEME_COLOR(member) .member = UI_RGB(UI_ICOUNT_RGB)
    const ui_mu_theme_t legacy_theme = {UI_ICOUNT_THEME_COLOR(panel_face),  UI_ICOUNT_THEME_COLOR(panel_edge),
                                        UI_ICOUNT_THEME_COLOR(button_face), UI_ICOUNT_THEME_COLOR(accent),
                                        UI_ICOUNT_THEME_COLOR(accent_face), UI_ICOUNT_THEME_COLOR(on_accent),
                                        UI_ICOUNT_THEME_COLOR(text),        UI_ICOUNT_THEME_COLOR(caption),
                                        UI_ICOUNT_THEME_COLOR(muted),       .text_scale = UI_ICOUNT_SCALE};
    int64_t elapsed = 0;
    for (int round = 0; round < UI_ICOUNT_ROUNDS; round++) {
        const int64_t began = timing_now_us();
        for (int rep = 0; rep < UI_ICOUNT_REPS; rep++) {
            ui_icount_state_t state = {.theme = legacy_theme, .value = UI_ICOUNT_VALUE};
            ui_begin(NULL);
            ui_theme(&theme);
            if (!window) {
                ui_screen();
                ui_row(box.h);
            }
            emit(&state);
            if (window && emit != face_screen && emit != face_window && emit != face_scroll_window) {
                mu_end_window(ui_context());
            }
            ui_face_end();
            mu_end(ui_context());
        }
        elapsed += timing_now_us() - began;
    }
    bench_icount_report(name, elapsed, UI_ICOUNT_REPS * UI_ICOUNT_ROUNDS, UI_ICOUNT_ROUNDS);
}

static void
test_ui_instruction_rows(void) {
    static const struct {
        const char* name;
        ui_icount_emit_fn emit;
        bool window;
    } rows[] = {
        {"ui_face_screen", face_screen, true},
        {"ui_mu_screen", legacy_screen, true},
        {"ui_face_window", face_window, true},
        {"ui_mu_window", legacy_window, true},
        {"ui_face_scroll_window", face_scroll_window, true},
        {"ui_face_row", face_row, false},
        {"ui_mu_row", legacy_row, false},
        {"ui_face_row_centre", face_row_centre, false},
        {"ui_face_next", face_next, false},
        {"ui_mu_next", legacy_next, false},
        {"ui_face_button", face_button, false},
        {"ui_mu_button", legacy_button, false},
        {"ui_face_tile", face_tile, false},
        {"ui_mu_tile", legacy_tile, false},
        {"ui_face_toggle", face_toggle, false},
        {"ui_mu_toggle", legacy_toggle, false},
        {"ui_face_slider", face_slider, false},
        {"ui_mu_slider", legacy_slider, false},
        {"ui_face_dropdown", face_dropdown, false},
        {"ui_mu_dropdown", legacy_dropdown, false},
        {"ui_face_dropdown_icons", face_dropdown_icons, false},
        {"ui_mu_dropdown_icons", legacy_dropdown_icons, false},
        {"ui_face_text", face_text, false},
        {"ui_mu_text", legacy_text, false},
        {"ui_face_text_right", face_text_right, false},
        {"ui_mu_text_right", legacy_text_right, false},
        {"ui_face_text_centre", face_text_centre, false},
        {"ui_mu_text_centre", legacy_text_centre, false},
        {"ui_face_text_height", face_text_height, false},
        {"ui_face_caption", face_caption, false},
        {"ui_mu_caption", legacy_caption, false},
        {"ui_face_header", face_header, false},
        {"ui_mu_header", legacy_header, false},
        {"ui_face_panel", face_panel, false},
        {"ui_mu_panel", legacy_panel, false},
        {"ui_face_fill", face_fill, false},
        {"ui_mu_fill", legacy_fill, false},
        {"ui_face_outline", face_outline, false},
        {"ui_mu_outline", legacy_outline, false},
        {"ui_face_icon", face_icon, false},
        {"ui_mu_icon", legacy_icon, false},
        {"ui_face_swatches", face_swatches, false},
        {"ui_mu_swatches", legacy_swatches, false},
        {"ui_face_theme", face_theme, false},
        {"ui_face_disabled", face_disabled, false},
        {"ui_face_color", face_color, false},
        {"ui_face_scale", face_scale, false},
        {"ui_face_alpha", face_alpha, false},
    };

    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        measure_row(rows[i].name, rows[i].emit, rows[i].window);
    }
}

void
run_ui_icount_suite(void) {
    RUN_TEST(test_ui_instruction_rows);
}

SUITE_REGISTER_ON_REQUEST(run_ui_icount_suite);
#endif
