/* sand_controls: how input reaches the simulation - brush radii, pour and step rates, catch-up and shake. */
#pragma once

#include "input/motion/gravity.h"

/* Flow strength uses Q8 unity at the simulation boundary. */
#define SAND_FLOW_ONE                GRAVITY_STRENGTH_ONE

/* Each mode's starting radius, in px, so a brush keeps its on-screen size
 * at every cell size. The brush screen's slider sets the value in force
 * (sand_ui_radius()). */
#define SAND_POUR_RADIUS_PX          10
#define SAND_ERASE_RADIUS_PX         16
#define SAND_DETONATE_RADIUS_PX      50

/* The default quality's cell size, in px. */
#define SAND_CELL_NORMAL             4

/* An erase removes emitters from wider than it sweeps: a point target needs
 * more aiming slack than an area does. */
#define SAND_ERASE_EMITTER_RADIUS_PX 32

#define SAND_POUR_HZ                 60
#define SAND_POUR_STEP_MS            (1000 / SAND_POUR_HZ)
#define SAND_STEP_HZ                 60
#define SAND_STEP_MS                 (1000 / SAND_STEP_HZ)

/* Steps or pours one frame may catch up on. More would turn a stall into a
 * visible lurch; fewer would slow the sand on a slow frame. */
#define SAND_MAX_CATCHUP             2

/* Shake level (0..1) to a jostle strength in 0..SAND_JOSTLE_MAX; out-of-range levels clamp. */
int sand_controls_jostle(float level);
