/* imu_odr: a requested sensor rate, applied by the IMU task between drains. */
#pragma once

#include <stdbool.h>

typedef struct {
    unsigned requested;
    unsigned applied;
} imu_odr_t;

typedef enum {
    IMU_ODR_UNCHANGED, /* nothing was asked */
    IMU_ODR_SWITCHED,  /* the requested rate runs */
    IMU_ODR_RESTORED,  /* the switch failed; the old rate runs again */
    IMU_ODR_LOST,      /* both failed; the sensor may be off */
} imu_odr_outcome_t;

/* Applies a pending request through `configure`. A failed switch can stop partway with the
 * sensor off, so the old rate is configured again and the request dropped. Every outcome
 * but IMU_ODR_UNCHANGED reset the FIFO. The caller queues that gap, so the next drain
 * must not repeat it through `fifo_gap`. */
static inline imu_odr_outcome_t
imu_odr_apply(imu_odr_t* odr, bool (*configure)(unsigned odr_code), bool* fifo_gap) {
    if (odr->requested == odr->applied) {
        return IMU_ODR_UNCHANGED;
    }
    *fifo_gap = false;
    if (configure(odr->requested)) {
        odr->applied = odr->requested;
        return IMU_ODR_SWITCHED;
    }
    odr->requested = odr->applied;
    return configure(odr->applied) ? IMU_ODR_RESTORED : IMU_ODR_LOST;
}
