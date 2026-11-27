/* capybara_run: the motion profile for a runner in Sponza's centimetres. */
#pragma once

#include "kinematics/profile/motion_profile.h"
#include "scene/entity/entity.h"

#define CAPYBARA_RUN_CM_PER_M 100.0F

float capybara_run_gallop_mps(entity_t runner);
motion_profile_t capybara_run_profile(float gallop_mps, float scale);
