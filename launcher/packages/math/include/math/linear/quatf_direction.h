/* quatf_direction: float rotations constructed from directions. */
#pragma once

#include "math/linear/quatf.h"

/* Aim local +Z with roll from up. Missing forward gives identity. For missing
 * or parallel up, use +Z or -Z according to forward.z, then +Y if needed. */
quatf_t quatf_looking(vec3f_t forward, vec3f_t up);

/* Shortest turn from a to b. Missing directions give identity. Near opposition,
 * turn halfway about the least-aligned basis axis crossed with a. */
quatf_t quatf_from_to(vec3f_t a, vec3f_t b);
