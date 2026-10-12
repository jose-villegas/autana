/* Capture tokens are shared with host parsers. */
#pragma once

#define CAPYBARA_PERF_PREFIX               "capybara_perf:"
#define CAPYBARA_PERF_PHASE_BEFORE_ENTER   "before_enter"
#define CAPYBARA_PERF_PHASE_LOADED         "loaded"
#define CAPYBARA_PERF_PHASE_PLAYING        "playing"
#define CAPYBARA_PERF_STAGE_ANIMATE        "animate"
#define CAPYBARA_PERF_STAGE_SKIN           "skin"
#define CAPYBARA_PERF_STAGE_ANIMATION_STEP "animation_step"
#define CAPYBARA_PERF_STAGE_RENDER         "render"
#define CAPYBARA_PERF_STAGE_FRAME          "frame"
#define CAPYBARA_PERF_STAGE_RENDER_DELTA   "render_delta"
#define CAPYBARA_PERF_STAGE_FRAME_DELTA    "frame_delta"

#define CAPYBARA_PERF_BUDGET_NS_PER_VERTEX 1000
#define CAPYBARA_PERF_SETUP_FORMAT         " joints=%d influences=%d lighting=%d lights=%d"
