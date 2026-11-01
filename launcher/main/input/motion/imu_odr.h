/* imu_odr: a requested sensor rate, applied by the IMU task between drains. */
#pragma once

#include <stdbool.h>
#include "input/motion/imu.h"
#include "input/motion/imu_demand.h"

typedef struct {
    unsigned requested;
    unsigned applied;
    uint32_t sequence;
    uint32_t served_sequence;
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
 * must not repeat it through `fifo_gap`. A configured FIFO also satisfies entry at `level`. */
static inline imu_odr_outcome_t
imu_odr_apply(imu_odr_t* odr, bool (*configure)(unsigned odr_code), bool* fifo_gap, imu_demand_level_t* level) {
    if (odr->requested == odr->applied) {
        return IMU_ODR_UNCHANGED;
    }
    *fifo_gap = false;
    if (configure(odr->requested)) {
        *level = IMU_DEMAND_FIFO;
        odr->applied = odr->requested;
        return IMU_ODR_SWITCHED;
    }
    odr->requested = odr->applied;
    if (configure(odr->applied)) {
        *level = IMU_DEMAND_FIFO;
        return IMU_ODR_RESTORED;
    }
    return IMU_ODR_LOST;
}

/* A rate alone is not an acknowledgement: FIFO entry must finish before a reader resumes. */
static inline bool
imu_odr_ready(const imu_odr_t* odr, uint32_t sequence) {
    return odr->sequence == sequence && odr->served_sequence == sequence;
}

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* Buzz-rate retained history bounds both polling and draining, independent of the saved tunes. */
static inline unsigned
imu_odr_period_ms(unsigned odr_code, unsigned requested_ms) {
    return odr_code == IMU_BUZZ_ODR_CODE && requested_ms > IMU_BUZZ_DRAIN_MAX_MS ? IMU_BUZZ_DRAIN_MAX_MS : requested_ms;
}
#endif
