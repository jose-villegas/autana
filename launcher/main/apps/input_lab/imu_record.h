/* imu_record: bounded round transcript retained until a new round or disposal. */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "apps/input_lab/imu_probe.h"
#include "apps/input_lab/shake_probe.h"

#define IMU_RECORD_CHUNK_SIZE  1024

#define IMU_RECORD_COMMAND     "labdump"
#define IMU_RECORD_PREFIX      "LABDUMP"
#define IMU_RECORD_TRUNCATED   IMU_RECORD_PREFIX "_TRUNCATED\n"
/* Fit payload: 55 label/separator bytes + nine signed 32-bit integers (11 each).
 * Prefix + space + newline + NUL frame it; margin permits label growth. */
#define IMU_RECORD_FIT_SIZE    (55 + (9 * 11))
#define IMU_RECORD_LINE_MARGIN 8
#define IMU_RECORD_LINE_SIZE   (IMU_RECORD_FIT_SIZE + sizeof(IMU_RECORD_PREFIX " ") + 1 + IMU_RECORD_LINE_MARGIN)
/* Start + fit, pose + gravity per position, and the decimated turn trace. */
#define IMU_RECORD_CAPACITY                                                                                            \
    (((2 + (2 * IMU_PROBE_POSE_COUNT) + IMU_PROBE_TURN_SAMPLES) * IMU_RECORD_LINE_SIZE) + sizeof IMU_RECORD_TRUNCATED)

#define IMU_RECORD_SHAKE_LINE_SIZE (SHAKE_PROBE_SAMPLE_SIZE + sizeof(IMU_RECORD_PREFIX " ") + IMU_RECORD_LINE_MARGIN)
#define IMU_RECORD_SHAKE_CAPACITY                                                                                      \
    ((SHAKE_PROBE_RECORD_LINES * IMU_RECORD_SHAKE_LINE_SIZE) + sizeof IMU_RECORD_TRUNCATED)

_Static_assert(IMU_RECORD_SHAKE_LINE_SIZE <= IMU_RECORD_LINE_SIZE, "shake payload must fit the shared formatter");

typedef struct {
    char* text;
    size_t capacity, used;
    bool truncated;
} imu_record_t;

/* Discards old text; fails below sizeof IMU_RECORD_TRUNCATED or on PSRAM exhaustion. */
bool imu_record_start(imu_record_t* record, size_t capacity);
/* Frees retained PSRAM and clears all state; safe on an empty record. */
void imu_record_free(imu_record_t* record);
/* Frames one payload with LABDUMP and newline. Reserves marker space; on overflow writes
 * the marker once and returns false. Empty records and later appends return false without writing. */
bool imu_record_appendf(imu_record_t* record, const char* format, ...);
/* Emits whole lines in bounded chunks. Text stays available until start or free. */
void imu_record_replay(const imu_record_t* record, void (*emit)(const char* text, size_t length));

/* NULL when ready to dump; otherwise the console error for a running or absent round. */
const char* imu_record_dump_error(const imu_record_t* record, imu_probe_step_t step);
