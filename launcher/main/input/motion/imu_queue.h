/* imu_queue: retained FIFO samples with ordered read-mode loss and gap reporting. */
#pragma once

#include "input/motion/imu.h"

typedef struct {
    imu_sample_t samples[IMU_FIFO_SAMPLES];
    /* Samples lost in FIFO read mode before each sample; saturation becomes a gap. */
    uint8_t lost_before[IMU_FIFO_SAMPLES];
#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
    int64_t sample_us[IMU_FIFO_SAMPLES];
#endif
    int head;
    int count;
    uint8_t carried_lost;
    bool gap;
    /* The queued samples' ODR code when it is not IMU_ODR_CODE (zero initialisation: the default). */
    bool other_rate;
    unsigned odr_code;
} imu_queue_t;

/* Zero initialisation creates an empty queue. A gap discards queued samples and carried loss,
 * so it precedes samples[0] of this push. */
void imu_queue_push(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap);
/* Copies out and empties the queue; *gap reports a discontinuity since the last take.
 * Takes at IMU_ODR_CODE: imu_queue_take_at() for another rate. */
int imu_queue_take(imu_queue_t* queue, imu_sample_t* samples, uint8_t* lost_before, bool* gap);
/* As imu_queue_take(), for samples queued at `odr_code`. Samples at another rate are
 * dropped behind a gap, never handed out, so no reader integrates them at the wrong period. */
int imu_queue_take_at(imu_queue_t* queue, unsigned odr_code, imu_sample_t* samples, uint8_t* lost_before, bool* gap);
/* Discards pre-restart history and uncounted read-mode loss behind one gap. */
void imu_queue_restart(imu_queue_t* queue, bool* fifo_gap, int* pending_lost);
/* Drops queued samples behind a gap; later pushes are at `odr_code`. */
void imu_queue_set_rate(imu_queue_t* queue, unsigned odr_code);

/* Maps failed drains to gaps and bounds successful drains to the scratch buffer capacity. */
void imu_queue_push_drain(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap);

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
/* The newest sample is placed at the IMU task's push time, independent of consumer polling. */
void imu_queue_push_drain_timed(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap,
                                int64_t newest_us, uint32_t sample_us);
int imu_queue_take_timed_at(imu_queue_t* queue, unsigned odr_code, imu_sample_t* samples, uint8_t* lost_before,
                            bool* gap, int64_t* sample_us);
#endif
