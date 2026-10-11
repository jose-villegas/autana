/* Authored fixture geometry must bake into a usable C table. */
#include "sample_layout_generated.h"

#define FIXTURE_FIRST_X      16
#define FIXTURE_FIRST_Y      16
#define FIXTURE_FIRST_WIDTH  120
#define FIXTURE_FIRST_HEIGHT 56

int
main(void) {
    const ui_layout_rect_t rect = sample_layout_portrait.rects[SAMPLE_ELEMENT_FIRST];
    return rect.x != FIXTURE_FIRST_X || rect.y != FIXTURE_FIRST_Y || rect.width != FIXTURE_FIRST_WIDTH
           || rect.height != FIXTURE_FIRST_HEIGHT;
}
