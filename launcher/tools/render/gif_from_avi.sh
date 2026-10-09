#!/bin/sh
# Two-pass GIF encoding for host-rendered AVI clips: a palette from FILTERS,
# then the clip through the same FILTERS onto that palette.
# Source from the repository root after scripts/lib/run.sh.
# gif_from_avi AVI GIF FILTERS [PALETTE_OPTIONS [SECONDS]]

gif_from_avi() {
    gif_avi=$1
    gif_out=$2
    gif_filters=$3
    gif_palette_options=${4:-stats_mode=diff}
    gif_palette=${gif_avi%.avi}-palette.png
    if [ -n "${5:-}" ]; then
        set -- -t "$5"
    else
        set --
    fi
    run ffmpeg -hide_banner -loglevel error -y "$@" -i "$gif_avi" \
        -vf "$gif_filters,palettegen=$gif_palette_options" "$gif_palette"
    run ffmpeg -hide_banner -loglevel error -y "$@" -i "$gif_avi" -i "$gif_palette" \
        -filter_complex "[0:v]${gif_filters}[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle" \
        -loop 0 "$gif_out"
}
