/* The labels perf suites print for each framebuffer layout they time, one spelling for every suite, so
 * the layout costs table (launcher/tools/render/layout_costs.py) joins their columns. */
#pragma once

#define PERF_LAYOUT_FULL_FB                 "FULL_FB"
#define PERF_LAYOUT_DOUBLE_FB               "DOUBLE_FB"
#define PERF_LAYOUT_DOUBLE_FB_CLEAR_ON_SEND "DOUBLE_FB_CLEAR_ON_SEND"
#define PERF_LAYOUT_DOUBLE_FB_CONTENDED     "DOUBLE_FB_CONTENDED"
