/* A settings frame owns its state and brackets its drawing. */
#include "ui/ui.h"

typedef struct {
    int volume;
    bool muted;
    int output;
    bool reset_requested;
} settings_state_t;

static const char* const OUTPUTS[] = {"SPEAKER", "HEADPHONES", "BLUETOOTH", "OFF"};
#define OUTPUT_COUNT (sizeof OUTPUTS / sizeof OUTPUTS[0])

static void
settings_draw(settings_state_t* s) {
    ui_screen();
    ui_row(UI_ROW_HEIGHT);
    ui_slider(ui_next(), "VOLUME", &s->volume, 0, 10);
    ui_row(UI_ROW_HEIGHT);
    ui_toggle(ui_next(), "MUTE", &s->muted);
    ui_row(UI_ROW_HEIGHT);
    ui_dropdown(ui_next(), "OUTPUT", &s->output, OUTPUTS, OUTPUT_COUNT);
    ui_row(UI_ROW_HEIGHT);
    if (ui_button(ui_next(), "RESET")) {
        s->reset_requested = true;
    }
}

void
settings_frame(const input_t* input, settings_state_t* s) {
    ui_begin(input);
    settings_draw(s);
    ui_end(UI_BACKGROUND);
}
