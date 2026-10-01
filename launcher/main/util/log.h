/*
 * log: lines for the console, tagged and levelled, so a caller above the
 * drivers reports without naming the logging library. Defined in
 * log_device.c, device only. A line is cut at LOG_LINE_MAX characters.
 */
#pragma once

#define LOG_LINE_MAX 192

void log_info(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
void log_warn(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
void log_error(const char* tag, const char* format, ...) __attribute__((format(printf, 2, 3)));
