/*
 * r3d: what a scene includes to draw a baked lit mesh: the camera,
 * raster, viewport and float vector.
 * Cull, transform and fill (r3d_pipeline.h, r3d_span.h) are render/'s own,
 * public only to its suites and host tools.
 */
#pragma once

#include "math/linear/vec3f.h"
#include "render/camera.h"
#include "render/r3d_lit_mesh.h"
#include "render/raster.h"
#include "render/viewport.h"
