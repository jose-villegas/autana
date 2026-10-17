/*
 * project_template: shared near-plane endpoint selection for pinhole clipping.
 * Each number type supplies its crossing arithmetic through P_cross_near.
 */
#pragma once

#include <stdbool.h>

#define MATH_DEFINE_CLIP_NEAR(P, V, T)                                                                                 \
    static inline bool P##_clip_near(T near_z, V* p0, V* p1) {                                                         \
        const bool front0 = p0->z > near_z;                                                                            \
        const bool front1 = p1->z > near_z;                                                                            \
        if (!front0 && !front1) {                                                                                      \
            return false;                                                                                              \
        }                                                                                                              \
        if (front0 != front1) {                                                                                        \
            V* behind = front0 ? p1 : p0;                                                                              \
            const V* front = front0 ? p0 : p1;                                                                         \
            P##_cross_near(near_z, behind, front);                                                                     \
        }                                                                                                              \
        return true;                                                                                                   \
    }
