#!/bin/sh
#
# Proves the clock read and the heap calls in util/timing.h and util/memory.h
# vanish into their callers on the board: built as the board builds them
# (ESP_PLATFORM, the IDF headers stood in for by stubs/), a caller's object
# names esp_timer_get_time, heap_caps_malloc and heap_caps_free and defines
# no timing_now_us, memory_alloc or memory_free. A frame of the module's own
# would cost the clock a call in its hottest loops, and would make every
# allocation one call site to a heap watch.
#
# Checked at -O0 as well as -O2, since only always_inline holds at -O0. The
# host compiler stands in for the board's; that the attribute holds there
# too is GCC's guarantee, not something this can see.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
MAIN_DIR="$HERE/../main"

CC_BIN=""
for c in cc gcc clang; do
    if command -v "$c" >/dev/null 2>&1; then CC_BIN="$c"; break; fi
done
if [ -z "$CC_BIN" ] || ! command -v nm >/dev/null 2>&1; then
    echo "check_inline_owners: no C compiler or nm found, skipping" >&2
    exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cat >"$work/caller.c" <<'SRC'
#include "util/memory.h"
#include "util/timing.h"

void* hold;
long long now;

void
caller(void) {
    now = timing_now_us();
    hold = memory_alloc(64, MEMORY_PSRAM);
    memory_free(hold);
}
SRC

status=0
for opt in -O0 -O2; do
    "$CC_BIN" -std=c11 -Wall -Wextra -Werror -DESP_PLATFORM $opt -I "$MAIN_DIR" -I "$HERE/stubs" \
        -c "$work/caller.c" -o "$work/caller.o"
    symbols=$(nm "$work/caller.o")
    for wanted in esp_timer_get_time heap_caps_malloc heap_caps_free; do
        if ! printf '%s\n' "$symbols" | grep -Eq "^ +U _?$wanted\$"; then
            echo "  FAIL $opt: the caller does not call $wanted itself"
            status=1
        fi
    done
    for wrapper in timing_now_us memory_alloc memory_free; do
        if printf '%s\n' "$symbols" | grep -Eq " _?$wrapper\$"; then
            echo "  FAIL $opt: $wrapper is a function of its own, not inlined"
            status=1
        fi
    done
done
[ "$status" -eq 0 ] && echo "  ok   timing.h and memory.h inline into their callers"
exit "$status"
