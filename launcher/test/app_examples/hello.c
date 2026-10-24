/* Hello: the shell calls enter, frame and exit. */
#include "app/app.h"
#include "draw/draw.h"

static void
yours_enter(void) {}

#define HELLO_BACKGROUND_RGB 0x101010
#define HELLO_TEXT_RGB       0xFFFFFF
#define HELLO_TEXT_X         20
#define HELLO_TEXT_Y         20

static void
yours_frame(uint32_t dt_ms, const input_t* input) {
    draw_clear(gfx_rgb(HELLO_BACKGROUND_RGB));
    draw_text(HELLO_TEXT_X, HELLO_TEXT_Y, "hello", gfx_rgb(HELLO_TEXT_RGB));
}

static void
yours_exit(void) {}

app_t app_yours = {
    .name = "Your App",
    .summary = "what it does",
    .enter = yours_enter,
    .frame = yours_frame,
    .exit = yours_exit,
    .home_gesture = true,
};

APP_REGISTER(app_yours);
