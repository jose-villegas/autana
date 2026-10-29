/* imu_demand: what the IMU task owes its readers, from when each kind last read. */
#pragma once

#include <stdint.h>

#include "input/motion/imu.h"

typedef enum {
    IMU_DEMAND_NONE,   /* no reader: the task parks */
    IMU_DEMAND_NEWEST, /* imu_read() only: register reads, no FIFO drains */
    IMU_DEMAND_FIFO,   /* FIFO takes: register reads and drains */
} imu_demand_level_t;

#define IMU_DEMAND_NEVER INT64_MIN

typedef struct {
    int64_t newest_us; /* last imu_read() */
    int64_t fifo_us;   /* last FIFO take or ODR request */
} imu_demand_t;

/* A reader keeps its level for the FIFO's retained history: a take later than
 * that finds a gap anyway, so stopping then costs it nothing. */
#define IMU_DEMAND_IDLE_US ((int64_t)IMU_FIFO_SAMPLES * IMU_SAMPLE_US)

static inline bool
imu_demand_recent(int64_t read_us, int64_t now_us) {
    return read_us != IMU_DEMAND_NEVER && now_us - read_us < IMU_DEMAND_IDLE_US;
}

static inline imu_demand_level_t
imu_demand_level(const imu_demand_t* demand, int64_t now_us) {
    if (imu_demand_recent(demand->fifo_us, now_us)) {
        return IMU_DEMAND_FIFO;
    }
    return imu_demand_recent(demand->newest_us, now_us) ? IMU_DEMAND_NEWEST : IMU_DEMAND_NONE;
}
