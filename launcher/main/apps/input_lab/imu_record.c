#include "apps/input_lab/imu_record.h"

#include <assert.h>
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

_Static_assert(IMU_RECORD_LINE_SIZE <= IMU_RECORD_CHUNK_SIZE, "each line must fit one output chunk");

void
imu_record_replay(const imu_record_t* record, void (*emit)(const char* text, size_t length)) {
    if (record == NULL) {
        return;
    }
    size_t offset = 0;
    while (offset < record->used) {
        size_t length = record->used - offset;
        if (length > IMU_RECORD_CHUNK_SIZE) {
            length = IMU_RECORD_CHUNK_SIZE;
        }
        while (length > 0 && record->text[offset + length - 1] != '\n') {
            length--;
        }
        assert(length > 0);
        emit(record->text + offset, length);
        offset += length;
    }
}

const char*
imu_record_dump_error(const imu_record_t* record, bool round_running) {
    if (round_running) {
        return IMU_RECORD_PREFIX "_ERR round active\n";
    }
    return record->text == NULL ? IMU_RECORD_PREFIX "_ERR no kept round\n" : NULL;
}
