#include "apps/input_lab/imu_record.h"

#include <stdlib.h>
#include <string.h>

void
imu_record_free(imu_record_t* record) {
    free(record->text);
    *record = (imu_record_t){0};
}

bool
imu_record_start(imu_record_t* record, size_t capacity) {
    imu_record_free(record);
    if (capacity < sizeof IMU_RECORD_TRUNCATED) {
        return false;
    }
    record->text = malloc(capacity);
    if (record->text == NULL) {
        return false;
    }
    record->text[0] = '\0';
    record->capacity = capacity;
    return true;
}

bool
imu_record_append(imu_record_t* record, const char* line) {
    if (record->text == NULL || record->truncated) {
        return false;
    }
    const size_t length = strlen(line);
    if (length > record->capacity - record->used - sizeof IMU_RECORD_TRUNCATED) {
        memcpy(record->text + record->used, IMU_RECORD_TRUNCATED, sizeof IMU_RECORD_TRUNCATED);
        record->used += sizeof IMU_RECORD_TRUNCATED - 1;
        record->truncated = true;
        return false;
    }
    memcpy(record->text + record->used, line, length + 1);
    record->used += length;
    return true;
}

void
imu_record_replay(imu_record_t* record, void (*emit)(const char* text)) {
    if (record->text != NULL) {
        emit(record->text);
    }
    imu_record_free(record);
}
