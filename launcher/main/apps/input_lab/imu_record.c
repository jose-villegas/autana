#include "apps/input_lab/imu_record.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "collections/strbuf.h"
#include "core/memory.h"

void
imu_record_free(imu_record_t* record) {
    memory_free(record->text);
    *record = (imu_record_t){0};
}

bool
imu_record_start(imu_record_t* record, size_t capacity) {
    imu_record_free(record);
    if (capacity < sizeof IMU_RECORD_TRUNCATED) {
        return false;
    }
    record->text = memory_alloc(capacity, MEMORY_PSRAM);
    if (record->text == NULL) {
        return false;
    }
    record->text[0] = '\0';
    record->capacity = capacity;
    return true;
}

bool
imu_record_appendf(imu_record_t* record, const char* format, ...) {
    if (record->text == NULL || record->truncated) {
        return false;
    }
    char line[IMU_RECORD_LINE_SIZE];
    strbuf_t text = strbuf_make(line, sizeof line);
    va_list args;
    va_start(args, format);
    const bool formed = strbuf_appendf(&text, IMU_RECORD_PREFIX " ") && strbuf_vappendf(&text, format, args)
                        && strbuf_appendf(&text, "\n");
    va_end(args);
    /* The marker's bytes stay free until a line does not fit. */
    strbuf_t kept = strbuf_at(record->text, record->capacity - (sizeof IMU_RECORD_TRUNCATED - 1), record->used);
    if (!formed || !strbuf_appendf(&kept, "%s", line)) {
        memcpy(record->text + record->used, IMU_RECORD_TRUNCATED, sizeof IMU_RECORD_TRUNCATED);
        record->used += sizeof IMU_RECORD_TRUNCATED - 1;
        record->truncated = true;
        return false;
    }
    record->used = kept.length;
    return true;
}

void
imu_record_take(imu_record_t* record, void (*emit)(const char* text)) {
    if (record->text != NULL) {
        emit(record->text);
    }
    imu_record_free(record);
}

const char*
imu_record_dump_error(const imu_record_t* record, imu_probe_step_t step) {
    if (step != IMU_PROBE_IDLE && step != IMU_PROBE_DONE) {
        return IMU_RECORD_PREFIX "_ERR round active\n";
    }
    return record->text == NULL ? IMU_RECORD_PREFIX "_ERR no kept round\n" : NULL;
}
