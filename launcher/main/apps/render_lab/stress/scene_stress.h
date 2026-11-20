/*
 * scene_stress: the stress presets (stress_preset.h), each a scene of the
 * Stress family keyed "stress-" and its preset's key. Its pack holds a
 * camera swinging round the origin and a pool of copies; the scene makes the
 * preset's mesh, shows it in as many copies as the preset asks for and hides
 * the rest.
 */
#pragma once

#include "apps/render_lab/stress/stress_preset.h"
#include "lab/lab_scene.h"
#include "math/scalar/math_const.h"

extern const lab_scene_t scene_stress[STRESS_PRESET_COUNT];

/* Wing excursion about its bias, in radians. */
#define STRESS_FLAP_AMPLITUDE  (55.0F * MATH_RADIANS_PER_DEGREE)
/* Mean wing angle, in radians. */
#define STRESS_FLAP_BIAS       (10.0F * MATH_RADIANS_PER_DEGREE)
/* Largest absolute wing angle, in radians. */
#define STRESS_FLAP_LIMIT      (STRESS_FLAP_AMPLITUDE + STRESS_FLAP_BIAS)

/* Flap cycles per second. */
#define STRESS_FLAP_HZ         4.0F
/* Angular phase between neighbouring copies, in radians. */
#define STRESS_FLAP_PHASE_STEP (MATH_PI * 0.618034F)

/* Frame-cost stage for prescribing all wing poses. */
#define STRESS_FLAP_STAGE_NAME "stress.flap"
