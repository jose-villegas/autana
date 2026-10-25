#include "input/motion/imu_queue.h"

void
imu_queue_push(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap) {
    if (gap || lost > UINT8_MAX - queue->carried_lost) {
        queue->gap = true;
        queue->head = queue->count = 0;
        queue->carried_lost = 0;
    } else {
        queue->carried_lost += (uint8_t)lost;
    }
    for (int i = 0; i < count; i++) {
        if (queue->count == IMU_FIFO_SAMPLES) {
            queue->head = (queue->head + 1) % IMU_FIFO_SAMPLES;
            queue->count--;
            queue->gap = true;
        }
        const int tail = (queue->head + queue->count) % IMU_FIFO_SAMPLES;
        queue->samples[tail] = samples[i];
        queue->lost_before[tail] = queue->carried_lost;
        queue->carried_lost = 0;
        queue->count++;
    }
}

int
imu_queue_take(imu_queue_t* queue, imu_sample_t* samples, uint8_t* lost_before, bool* gap) {
    const int count = queue->count;
    for (int i = 0; i < count; i++) {
        const int index = (queue->head + i) % IMU_FIFO_SAMPLES;
        samples[i] = queue->samples[index];
        lost_before[i] = queue->lost_before[index];
    }
    *gap = queue->gap;
    queue->gap = false;
    queue->head = 0;
    queue->count = 0;
    return count;
}

void
imu_queue_push_drain(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap) {
    if (count < 0) {
        gap = true;
        count = 0;
    } else if (count > IMU_FIFO_SAMPLES) {
        count = IMU_FIFO_SAMPLES;
    }
    imu_queue_push(queue, samples, count, lost, gap);
}
