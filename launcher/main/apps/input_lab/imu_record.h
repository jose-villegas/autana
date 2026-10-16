/* imu_record: bounded round transcript retained until replay or disposal. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "apps/input_lab/imu_probe.h"

#define IMU_RECORD_COMMAND   "labdump"
#define IMU_RECORD_PREFIX    "LABDUMP"
#define IMU_RECORD_TRUNCATED IMU_RECORD_PREFIX "_TRUNCATED\n"
/* The longest fit line has nine signed integers, labels and framing. */
#define IMU_RECORD_LINE_SIZE 256
/* Start + fit, pose + gravity per position, and the decimated turn trace. */
#define IMU_RECORD_CAPACITY                                                                                            \
    ((2 + 2 * IMU_PROBE_POSE_COUNT + IMU_PROBE_TURN_SAMPLES) * IMU_RECORD_LINE_SIZE + sizeof IMU_RECORD_TRUNCATED)

typedef struct {
    char* text;
    size_t capacity, used;
    bool truncated;
} imu_record_t;

bool imu_record_start(imu_record_t* record, size_t capacity);
void imu_record_free(imu_record_t* record);
bool imu_record_append(imu_record_t* record, const char* line);
void imu_record_replay(imu_record_t* record, void (*emit)(const char* text));
