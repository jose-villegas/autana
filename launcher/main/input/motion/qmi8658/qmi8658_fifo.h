/* qmi8658_fifo: discontinuities and delayed read-mode loss between chip drains. */
#pragma once

#include "input/motion/imu.h"

typedef struct {
    bool gap;
    int pending_lost;
    unsigned odr_code;
} qmi8658_fifo_t;

static inline void
qmi8658_fifo_reset(qmi8658_fifo_t* fifo, unsigned odr_code) {
    *fifo = (qmi8658_fifo_t){.odr_code = odr_code};
}

/* Read-mode losses follow this burst, so the consumer advances them with the next batch. */
static inline int
qmi8658_fifo_loss(qmi8658_fifo_t* fifo, uint64_t locked_us, bool gap) {
    const int lost = gap ? 0 : fifo->pending_lost;
    fifo->pending_lost = imu_fifo_lost(locked_us, imu_odr_period_us(fifo->odr_code), gap);
    return lost;
}
