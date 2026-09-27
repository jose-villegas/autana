/*
 * sponza_flythrough - the camera loop through Sponza's atrium: in from the
 * east arcade at eye height, down the atrium, up past the galleries and
 * back. Model units are centimetres; y is up.
 */
#pragma once

#include "camera_path.h"

/* The camera keeps at least this far from every triangle, so the near
 * plane never cuts into a wall; suite_sponza.c holds the path to it. */
#define SPONZA_FLYTHROUGH_CLEARANCE 25.0f

extern const camera_path_t sponza_flythrough;
