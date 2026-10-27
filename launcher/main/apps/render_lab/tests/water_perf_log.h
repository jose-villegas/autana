/* The water perf suite's log lines, each WATER_PERF_PREFIX and a space
 * then a format below; tools/water_tables.py reads these formats, so a
 * field renamed here is renamed there. */
#pragma once

#define WATER_PERF_PREFIX   "water_perf:"

/* Fixed simulated time per frame: the tour plays the same water every run,
 * so runs differ only in timing. */
#define WATER_PERF_FRAME_MS 33

/* Two per configuration and tour phase: its counts, then the frame, the
 * update (simulation and surface) and r3d drawing the surface, each
 * mean/p95/worst in us. */
#define WATER_PERF_ROW      "config=%s row=%s frames=%d substeps=%u rested=%u tris=%lld particles=%d peak_mm_s=%.1f"
#define WATER_PERF_TIMES    "config=%s row=%s frame=%lld/%lld/%lld update=%lld/%lld/%lld draw=%lld/%lld/%lld"

/* One per stage of a row: mean us per frame. kind is WATER_PERF_SCENE for
 * the update's stages and WATER_PERF_FLIP3 for the step's own. */
#define WATER_PERF_STAGE    "config=%s row=%s kind=%s stage=%s us=%lld"
#define WATER_PERF_SCENE    "scene"
#define WATER_PERF_FLIP3    "flip3"

/* frame_cost's report over a row, after this prefix: the r3d stages. */
#define WATER_PERF_COST     "config=%s row=%s ms/frame avg/worst: %s"

/* Internal RAM around a configuration: before the scene enters, while it
 * runs and after it exits. */
#define WATER_PERF_HEAP     "config=%s phase=%s internal_free=%u internal_largest=%u"
#define WATER_PERF_BEFORE   "before"
#define WATER_PERF_RUNNING  "running"
#define WATER_PERF_AFTER    "after"
