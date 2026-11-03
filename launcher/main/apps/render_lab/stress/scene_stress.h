/*
 * scene_stress: the stress presets (stress_preset.h), each a scene of the
 * Stress family keyed "stress-" and its preset's key. Its pack holds a
 * camera swinging round the origin and a pool of copies; the scene makes the
 * preset's mesh, shows it in as many copies as the preset asks for and hides
 * the rest.
 */
#pragma once

#include "apps/render_lab/render_lab_scene.h"
#include "apps/render_lab/stress/stress_preset.h"

extern const render_lab_scene_t scene_stress[STRESS_PRESET_COUNT];
