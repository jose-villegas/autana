#include "input/imu_rate.h"

#define PERCENT_SCALE 100U

uint32_t
imu_rate_update(imu_rate_t* rate, int samples, int64_t drain_us, bool gap) {
    if (rate->sample_us == 0) {
        rate->sample_us = IMU_SAMPLE_US;
    }
    if (gap || samples < 0) {
        rate->active = false;
        rate->samples = 0;
        return rate->sample_us;
    }
    if (!rate->active || drain_us <= rate->start_us) {
        rate->active = true;
        rate->start_us = drain_us;
        rate->samples = 0;
        return rate->sample_us;
    }
    rate->samples += (uint32_t)samples;
    const int64_t elapsed = drain_us - rate->start_us;
    if (elapsed >= IMU_RATE_WINDOW_US) {
        if (rate->samples > 0) {
            const int64_t estimate = (elapsed + (rate->samples / 2U)) / rate->samples;
            if (estimate * PERCENT_SCALE >= IMU_SAMPLE_US * (PERCENT_SCALE - IMU_RATE_BOUND_PCT)
                && estimate * PERCENT_SCALE <= IMU_SAMPLE_US * (PERCENT_SCALE + IMU_RATE_BOUND_PCT)) {
                rate->sample_us = (uint32_t)estimate;
            }
        }
        rate->start_us = drain_us;
        rate->samples = 0;
    }
    return rate->sample_us;
}
