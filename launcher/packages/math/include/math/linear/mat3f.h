/* Row-major linear matrices and their normal transforms. */
#pragma once
#include "math/linear/mat4f.h"

enum { MAT3F_AXES = 3 };

typedef struct {
    float m[MAT3F_AXES][MAT3F_AXES];
} mat3f_t;

static inline __attribute__((always_inline)) mat3f_t
mat3f_from_mat4(mat4f_t matrix) {
    mat3f_t out;
#pragma GCC unroll MAT3F_AXES
    for (int row = 0; row < MAT3F_AXES; row++) {
#pragma GCC unroll MAT3F_AXES
        for (int col = 0; col < MAT3F_AXES; col++) {
            out.m[row][col] = matrix.m[row][col];
        }
    }
    return out;
}

static inline __attribute__((always_inline)) mat3f_t
mat3f_mul(mat3f_t a, mat3f_t b) {
    mat3f_t out;
#pragma GCC unroll MAT3F_AXES
    for (int row = 0; row < MAT3F_AXES; row++) {
#pragma GCC unroll MAT3F_AXES
        for (int col = 0; col < MAT3F_AXES; col++) {
            out.m[row][col] = a.m[row][0] * b.m[0][col] + a.m[row][1] * b.m[1][col] + a.m[row][2] * b.m[2][col];
        }
    }
    return out;
}

static inline __attribute__((always_inline)) mat3f_t
mat3f_cofactor(mat3f_t matrix) {
    mat3f_t out;
#pragma GCC unroll MAT3F_AXES
    for (int row = 0; row < MAT3F_AXES; row++) {
        const int a = (row + 1) % MAT3F_AXES, b = (row + 2) % MAT3F_AXES;
#pragma GCC unroll MAT3F_AXES
        for (int col = 0; col < MAT3F_AXES; col++) {
            const int c = (col + 1) % MAT3F_AXES, d = (col + 2) % MAT3F_AXES;
            out.m[row][col] = matrix.m[a][c] * matrix.m[b][d] - matrix.m[a][d] * matrix.m[b][c];
        }
    }
    return out;
}

/* Negative when the matrix mirrors, zero when it is singular. */
static inline __attribute__((always_inline)) float
mat3f_determinant(mat3f_t matrix) {
    const mat3f_t cofactor = mat3f_cofactor(matrix);
    return (matrix.m[0][0] * cofactor.m[0][0]) + (matrix.m[0][1] * cofactor.m[0][1])
           + (matrix.m[0][2] * cofactor.m[0][2]);
}
