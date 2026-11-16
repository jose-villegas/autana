#include "apps/physics_lab/scene_stage.h"
#include "draw/draw.h"
#include "image/pixel/rgb565.h"
#include "ui/ui.h"

/* The empty stage that physics scenes replace. */
static void
stage_enter(const lab_scene_t* self) {
    (void)self;
}

/* Nothing composes under an empty stage, so each frame repaints it: the
 * BOOT menu and the HUD leave nothing behind. */
static void
stage_frame(uint32_t dt_ms) {
    (void)dt_ms;
    draw_clear(RGB565BE(UI_BACKGROUND));
}

static void
stage_exit(void) {}

const lab_scene_t scene_physics_stage = {
    .name = "Stage",
    .key = PHYSICS_STAGE_KEY,
    .family = "Stage",
    .enter = stage_enter,
    .frame = stage_frame,
    .exit = stage_exit,
};
