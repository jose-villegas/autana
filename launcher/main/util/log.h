/*
 * log: lines for the console, tagged and levelled, so a caller above the
 * drivers reports without naming the logging library. Defined in
 * log_device.c, device only, and a line is written whole however long it is.
 * A device half beside a driver may still call the logging library directly.
 */
#pragma once

void log_info(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
void log_warn(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
void log_error(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
