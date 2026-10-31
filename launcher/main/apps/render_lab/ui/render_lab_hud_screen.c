#include "render_lab_hud_screen.h"

#include <stdio.h>

#include "apps/render_lab/render_lab.h"
#include "draw/draw.h"
#include "ui/ui.h"

/* The panel's bezel hides about 15 px along every edge. */
#define HUD_INSET_PX    16
#define HUD_LINE_GAP_PX 4
/* The backing's margin around its text, on each side. */
#define HUD_BOX_PAD_X   4
#define HUD_BOX_PAD_Y   2

static recti_t
text_box(const char* text, int x, int y) {
    return (recti_t){x, y, draw_text_width(text, -1) + (2 * HUD_BOX_PAD_X), draw_text_height() + (2 * HUD_BOX_PAD_Y)};
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* Opaque backing keeps HUD text legible over the scene. */
static recti_t
draw_text_box(const char* text, int x, int y) {
    const recti_t box = text_box(text, x, y);
    ui_fill(box, UI_BACKGROUND);
    ui_text_centre(box, text);
    return box;
}
#endif

/* Centred on the highest HUD row whose corner box it clears: `row_right[i]`
 * is where row i's box ends, 0 for an empty row. The backing stays opaque
 * to keep fading text legible over the scene. */
#define HUD_CORNER_ROWS 2

static void
draw_scene_title(const char* title, uint8_t alpha, const int row_right[HUD_CORNER_ROWS]) {
    if (alpha == 0) {
        return;
    }
    recti_t box = text_box(title, 0, HUD_INSET_PX);
    box.x = (ui_width() - box.w) / 2;

    int row = 0;
    while (row < HUD_CORNER_ROWS && box.x < row_right[row] + HUD_LINE_GAP_PX) {
        row++;
    }
    box.y = HUD_INSET_PX + (row * (box.h + HUD_LINE_GAP_PX));
    ui_fill(box, UI_BACKGROUND);
    ui_alpha_begin(alpha);
    ui_text_centre(box, title);
    ui_alpha_end();
}

void
render_lab_hud_screen_draw(const render_lab_hud_screen_state_t* state) {
    /* An outline keeps text readable over a changing scene. */
    ui_set_text_style(UI_TEXT_OUTLINED);
    ui_screen();

    int row_right[HUD_CORNER_ROWS] = {0};
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    char fps_line[16];
    (void)snprintf(fps_line, sizeof fps_line, "%5.1f fps", (double)state->fps_value);

    const int x = HUD_INSET_PX;
    const recti_t fps_box = draw_text_box(fps_line, x, HUD_INSET_PX);
    /* The fps row is reserved at its widest reading, so the title does
     * not change rows as the number gains a digit. */
    row_right[0] = recti_right(text_box("999.9 fps", x, 0));
    if (state->status != NULL) {
        const recti_t status_box = draw_text_box(state->status, x, recti_bottom(fps_box) + HUD_LINE_GAP_PX);
        row_right[1] = recti_right(status_box);
    }
#endif
    draw_scene_title(state->scene_title, state->scene_title_alpha, row_right);
}
