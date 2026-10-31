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

void
imu_queue_restart(imu_queue_t* queue, bool* fifo_gap, int* pending_lost, uint32_t* remainder_us) {
    *fifo_gap = false;
    *pending_lost = 0;
    *remainder_us = 0;
    imu_queue_push(queue, NULL, 0, 0, true);
}

void
imu_queue_set_rate(imu_queue_t* queue, unsigned odr_code) {
    imu_queue_push(queue, NULL, 0, 0, true);
    queue->other_rate = odr_code != IMU_ODR_CODE;
    queue->odr_code = odr_code;
}

int
imu_queue_take(imu_queue_t* queue, imu_sample_t* samples, uint8_t* lost_before, bool* gap) {
    return imu_queue_take_at(queue, IMU_ODR_CODE, samples, lost_before, gap);
}

int
imu_queue_take_at(imu_queue_t* queue, unsigned odr_code, imu_sample_t* samples, uint8_t* lost_before, bool* gap) {
    const bool rate_matches = queue->other_rate ? queue->odr_code == odr_code : odr_code == IMU_ODR_CODE;
    if (!rate_matches) {
        imu_queue_push(queue, NULL, 0, 0, true);
        *gap = true;
        return 0;
    }
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

#if BUILD_VARIANT_DEVELOPMENT_OR_HOST
void
imu_queue_push_drain_timed(imu_queue_t* queue, const imu_sample_t* samples, int count, int lost, bool gap,
                           int64_t newest_us, uint32_t sample_us) {
    imu_queue_push_drain(queue, samples, count, lost, gap);
    const int retained = count < 0 ? 0 : count > IMU_FIFO_SAMPLES ? IMU_FIFO_SAMPLES : count;
    for (int i = 0; i < retained; i++) {
        const int index = (queue->head + queue->count - retained + i) % IMU_FIFO_SAMPLES;
        queue->sample_us[index] = newest_us - ((int64_t)(retained - i - 1) * sample_us);
    }
}

int
imu_queue_take_timed_at(imu_queue_t* queue, unsigned odr_code, imu_sample_t* samples, uint8_t* lost_before, bool* gap,
                        int64_t* sample_us) {
    for (int i = 0; i < queue->count; i++) {
        sample_us[i] = queue->sample_us[(queue->head + i) % IMU_FIFO_SAMPLES];
    }
    return imu_queue_take_at(queue, odr_code, samples, lost_before, gap);
}
#endif
