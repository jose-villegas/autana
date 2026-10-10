#!/usr/bin/env python3
"""Public track sampler; bake-time sampling is owned by track_host."""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))

from anim import track_host  # noqa: E402


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if "--poses" in argv and not (argv and argv[0].endswith(track_host.tracks_asset.SUFFIX)):
        print("sample_tracks: --poses requires a .anim.toml in source space, not --pack", file=sys.stderr)
        return 2
    return track_host.main(argv)


if __name__ == "__main__":
    sys.exit(main())
