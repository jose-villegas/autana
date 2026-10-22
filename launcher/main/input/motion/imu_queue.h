#pragma once

#include "input/motion/imu.h"

typedef struct {
    imu_sample_t samples[IMU_FIFO_SAMPLES];
    uint8_t lost_before[IMU_FIFO_SAMPLES];
    int head;
    int count;
    uint8_t carried_lost;
    bool gap;
} imu_queue_t;

/* Zero initialisation creates an empty queue. A gap invalidates carried loss. */
void imu_queue_push(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap);
int imu_queue_take(imu_queue_t* queue, imu_sample_t* samples, uint8_t* lost_before, bool* gap);
