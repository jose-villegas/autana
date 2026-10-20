/* The skin perf suite's log tokens, setup format and skin budget, read by launcher/main/apps/render_lab/tools/report_performance.py. */
#pragma once

#define SKIN_PERF_PREFIX               "capybara_perf:"
#define SKIN_PERF_PHASE_BEFORE_ENTER   "before_enter"
#define SKIN_PERF_PHASE_LOADED         "loaded"
#define SKIN_PERF_PHASE_PLAYING        "playing"
#define SKIN_PERF_STAGE_ANIMATE        "animate"
#define SKIN_PERF_STAGE_SKIN           "skin"
#define SKIN_PERF_STAGE_ANIMATION_STEP "animation_step"
#define SKIN_PERF_STAGE_RENDER         "render"
#define SKIN_PERF_STAGE_FRAME          "frame"
#define SKIN_PERF_STAGE_RENDER_DELTA   "render_delta"
#define SKIN_PERF_STAGE_FRAME_DELTA    "frame_delta"

#define SKIN_PERF_BUDGET_NS_PER_VERTEX 1000
#define SKIN_PERF_NS_PER_US            1000
#define SKIN_PERF_CORES_ONE            "1"
#define SKIN_PERF_CORES_TWO            "2"
#define SKIN_PERF_CORES_UNKNOWN        "unknown"
#define SKIN_PERF_SETUP_FORMAT         " joints=%d influences=%d lighting=%d lights=%d"
