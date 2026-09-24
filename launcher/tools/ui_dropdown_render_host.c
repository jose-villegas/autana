/*
 * ui_dropdown_render_host - ui_dropdown() on its own, driven by a scripted
 * finger: open it, drag its list down and back up, pick another item, then
 * all of it again from the new pick. Twelve items near the bottom of the
 * canvas, so the list opens upward and has to scroll.
 *
 * Touch comes from the script below by frame, not the harness's declared
 * steps: a drag needs a new position every frame, which a step table would
 * spell out one line per frame.
 */

#include <stdbool.h>
#include <stdio.h>

#include "gfx/gfx.h"
#include "render_host.h"
#include "ui/ui.h"
#include "ui/ui_transform.h"
#include "ui/ui_widgets.h"

#define BACKGROUND 0x0A0C14
#define ITEM_COUNT 12

static const ui_theme_t THEME = {
    .panel_face = UI_RGB(0x131C2E),
    .panel_edge = UI_RGB(0xE8ECF4),
    .button_face = UI_RGB(0x1B2740),
    .accent = UI_RGB(0xE0A63C),
    .accent_face = UI_RGB(0xE0A63C),
    .on_accent = UI_RGB(0x2A1A06),
    .text = UI_RGB(0xF2F6FF),
    .caption = UI_RGB(0x8FA3C0),
    .muted = UI_RGB(0x4A5672),
    .text_scale = 2,
    .icon_side = 32,
};

static const mu_Rect DROPDOWN = {40, 360, 288, UI_TAP_MIN};

static char labels[ITEM_COUNT][16];
static ui_dropdown_item_t items[ITEM_COUNT];
static int selected = 2;

int
display_shell_quarter(void) {
    return 0;
}

static bool
setup(int quarter) {
    for (int i = 0; i < ITEM_COUNT; i++) {
        snprintf(labels[i], sizeof labels[i], "OPTION %02d", i + 1);
        items[i] = (ui_dropdown_item_t){.label = labels[i]};
    }
    ui_init();
    ui_set_transform(ui_transform_quarter_turn(quarter, GFX_WIDTH, GFX_HEIGHT));
    return true;
}

typedef struct {
    int start; /* first frame the finger is down */
    int frames;
    int x0, y0, x1, y1;
} stroke_t;

#define FACE_X (DROPDOWN.x + DROPDOWN.w / 2)
#define FACE_Y (DROPDOWN.y + DROPDOWN.h / 2)

/* Taps hold four frames: ui_pointer waits out microui's hover lag. */
static const stroke_t SCRIPT[] = {
    {15, 4, FACE_X, FACE_Y, FACE_X, FACE_Y},
    {40, 24, 184, 300, 184, 70},
    {80, 24, 184, 70, 184, 300},
    {120, 4, 184, 200, 184, 200},
    {150, 4, FACE_X, FACE_Y, FACE_X, FACE_Y},
    {175, 24, 184, 300, 184, 70},
    {215, 24, 184, 70, 184, 300},
    {255, 4, 184, 120, 184, 120},
};

static input_t
scripted(int frame) {
    input_t in = {0};
    for (size_t i = 0; i < sizeof SCRIPT / sizeof SCRIPT[0]; i++) {
        const stroke_t* s = &SCRIPT[i];
        const int t = frame - s->start;
        if (t < 0 || t > s->frames) {
            continue;
        }
        in.x = s->x0 + (s->x1 - s->x0) * t / s->frames;
        in.y = s->y0 + (s->y1 - s->y0) * t / s->frames;
        in.down = t < s->frames;
        in.pressed = t == 0;
        in.released = t == s->frames;
    }
    return in;
}

static void
draw(const render_frame_t* frame) {
    const input_t in = scripted(frame->index);
    ui_begin(&in);
    ui_set_text_style(UI_TEXT_PLAIN);
    if (ui_begin_screen(ui_context(), "Dropdown", MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOCLOSE | MU_OPT_NOFRAME)) {
        ui_header_bar(ui_context(), mu_rect(0, 0, ui_width(), 36), "DROPDOWN", 2, &THEME);
        const int picked = ui_dropdown(ui_context(), "demo", DROPDOWN, items, ITEM_COUNT, selected, &THEME);
        if (picked >= 0) {
            selected = picked;
        }
        mu_end_window(ui_context());
    }
    ui_end(BACKGROUND);
}

const render_scene_t render_scene = {
    .name = "ui_dropdown",
    .quarter = 0,
    .frames = 290,
    .dt_ms = 33,
    .setup = setup,
    .draw = draw,
};
