/* imu_queue: retained FIFO samples with ordered read-mode loss and gap reporting. */
#pragma once

#include "input/motion/imu.h"

typedef struct {
    imu_sample_t samples[IMU_FIFO_SAMPLES];
    /* Samples lost in FIFO read mode before each sample; saturation becomes a gap. */
    uint8_t lost_before[IMU_FIFO_SAMPLES];
    int head;
    int count;
    uint8_t carried_lost;
    bool gap;
} imu_queue_t;

/* Zero initialisation creates an empty queue. A gap discards queued samples and carried loss,
 * so it precedes samples[0] of this push. */
void imu_queue_push(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap);
/* Copies out and empties the queue; *gap reports a discontinuity since the last take. */
int imu_queue_take(imu_queue_t* queue, imu_sample_t* samples, uint8_t* lost_before, bool* gap);

/* Maps failed drains to gaps and bounds successful drains to the scratch buffer capacity. */
void imu_queue_push_drain(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap);
