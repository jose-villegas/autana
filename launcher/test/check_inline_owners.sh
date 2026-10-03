#!/bin/sh
#
# Proves the clock read and the heap calls in util/timing.h and util/memory.h
# vanish into their callers, built as the board builds them (ESP_PLATFORM,
# the IDF headers stood in for by stubs/) and as a host render does (the C
# library's heap): a caller's object names the underlying calls itself and
# defines no timing_now_us, memory_alloc or memory_free. A frame of the
# module's own would cost the clock a call in its hottest loops, and would
# make every allocation one call site to a heap watch.
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
# platform|extra flags|what the caller must call itself, as EREs (MinGW's
# timespec_get is a macro for _timespec64_get)
for build in "board|-DESP_PLATFORM|esp_timer_get_time heap_caps_malloc heap_caps_free" \
    "render||timespec(64)?_get malloc free"; do
    name=${build%%|*}
    rest=${build#*|}
    defines=${rest%%|*}
    wanted_list=${rest#*|}
    for opt in -O0 -O2; do
        # shellcheck disable=SC2086
        "$CC_BIN" -std=c11 -Wall -Wextra -Werror $defines $opt -I "$MAIN_DIR" -I "$HERE/stubs" \
            -c "$work/caller.c" -o "$work/caller.o"
        symbols=$(nm "$work/caller.o")
        for wanted in $wanted_list; do
            if ! printf '%s\n' "$symbols" | grep -Eq "^ +U _?$wanted\$"; then
                echo "  FAIL $name $opt: the caller does not call $wanted itself"
                status=1
            fi
        done
        # A leading dot is a section symbol (MinGW's nm lists .text), not code.
        if printf '%s\n' "$symbols" | grep -Eq "^[0-9a-f]+ [tT] [^.]"; then
            for wrapper in $(printf '%s\n' "$symbols" | awk '$2 ~ /^[tT]$/ && $3 != "caller" && $3 !~ /^\./ { print $3 }'); do
                echo "  FAIL $name $opt: $wrapper is a function of its own, not inlined"
                status=1
            done
        fi
    done
done
[ "$status" -eq 0 ] && echo "  ok   timing.h and memory.h inline into their callers"
exit "$status"
