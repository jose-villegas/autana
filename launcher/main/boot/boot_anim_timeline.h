/* GENERATED FILE - do not edit. Run tools/gen/gen_boot_anim_timeline.py. */
#pragma once

#include <stdint.h>

#define BOOT_ANIM_MS 5500

#define BOOT_ANIM_AXES_MS 450

#define BOOT_ANIM_GRID_START_MS 320
#define BOOT_ANIM_GRID_RING_MS 16

#define BOOT_ANIM_GRID_RINGS 128

/* Zero hides radial guide lines. */
#define BOOT_ANIM_GRID_SPOKES 4

/* Zero draws solid spokes; one draws dashed spokes. */
#define BOOT_ANIM_GRID_SPOKE_DASH 1

#define BOOT_ANIM_GRID_SPOKE_START_MS 2300

/* Zero draws the full spoke length immediately. */
#define BOOT_ANIM_GRID_SPOKE_DRAW_MS 800

#define BOOT_ANIM_GRID_FADE_MS 128
#define BOOT_ANIM_WAVE_IN_MS 750

#define BOOT_ANIM_WAVE_OUT_MS 5500

#define BOOT_ANIM_PEN_START_MS 520
#define BOOT_ANIM_PEN_MS 1980

#define BOOT_ANIM_PEN_FINISH_MS 4800

#define BOOT_ANIM_TITLE_START_MS 2450

#define BOOT_ANIM_TITLE_STAGGER_MS 170

#define BOOT_ANIM_TITLE_FLIGHT_MS 500

#define BOOT_ANIM_TITLE_ENTRY_PX 420

#define BOOT_ANIM_TITLE_TURNS_PHASE 92000

#define BOOT_ANIM_TITLE_AMPLITUDE_PX 15

#define BOOT_ANIM_TITLE_WAVE_AMPLITUDE_PX 12

#define BOOT_ANIM_TITLE_WAVE_PERIOD_MS 900
#define BOOT_ANIM_TITLE_WAVE_STAGGER_MS 75
#define BOOT_ANIM_TITLE_SCALE 5

#define BOOT_ANIM_TITLE_WAVE_OUT_MS 3200

#define BOOT_ANIM_TITLE_WAVE_FADE_MS 1500

#define BOOT_ANIM_TITLE_VIEW_Y 40

#define BOOT_ANIM_TITLE_VIEW_X 95

/* A zero offset disables the shadow. */
#define BOOT_ANIM_TITLE_SHADOW_DX 5

#define BOOT_ANIM_TITLE_SHADOW_DY 5

/* Shadow opacity ranges from 0 (invisible) to 255 (solid). */
#define BOOT_ANIM_TITLE_SHADOW_ALPHA 128

#define BOOT_ANIM_IMAGE_START_MS 3500

/* Zero makes the photograph transition immediate. */
#define BOOT_ANIM_IMAGE_FADE_MS 800

#define BOOT_ANIM_FADE_START_MS 4800

#define BOOT_ANIM_GRID_HUE_MS 700

#define BOOT_ANIM_GRID_HUE_SPREAD 64

#define BOOT_ANIM_GRID_WHITEN_MAX 32
#define BOOT_ANIM_GRID_CEILING_MAX 96
#define BOOT_ANIM_GRID_MAX 64
/* Zero is orthographic; other values are perspective focal lengths. */
#define BOOT_ANIM_CAMERA_FOCAL 1.0F

/* Floor-ring spacing in Q12 metres. */
#define BOOT_ANIM_GRID_STEP_Q12 410

/* Peak wave amplitude in Q12 metres; zero disables the ripple. */
#define BOOT_ANIM_WAVE_HEIGHT_Q12 5120

/* Crest-to-crest distance in Q12 metres. */
#define BOOT_ANIM_WAVE_WAVELENGTH_Q12 20480

/* Wave period in milliseconds. */
#define BOOT_ANIM_WAVE_PERIOD_MS 800
