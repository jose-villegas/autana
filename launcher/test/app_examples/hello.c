/* Hello: the shell calls enter, frame and exit. */
#include "app/app.h"
#include "draw/draw.h"

static void
yours_enter(void) { /* reset state */ }

static void
yours_frame(uint32_t dt_ms, const input_t* input) {
    draw_clear(gfx_rgb(0x101010));
    draw_text(20, 20, "hello", gfx_rgb(0xFFFFFF));
}

static void
yours_exit(void) { /* release what enter() took */ }

app_t app_yours = {
    .name = "Your App",
    .summary = "what it does",
    .enter = yours_enter,
    .frame = yours_frame,
    .exit = yours_exit,
    .home_gesture = true,
};

APP_REGISTER(app_yours);
