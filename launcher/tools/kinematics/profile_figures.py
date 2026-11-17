#!/usr/bin/env python3
"""Plot a full and a short motion profile's place and speed for docs/kinematics/Profile.md; --cc CC --out DIR."""
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "math"))
from math_figures import PANEL_SIZE, ROOT
from docs.figures import probe_into_folder
from docs.producer import Producer
from docs.plot_style import FAINT, LABEL_SIZE, SLOTS, save, subplots
import numpy as np

PROBE = pathlib.Path(__file__).with_name("profile_figures_probe.c")
SOURCES = (PROBE, ROOT / "launcher/packages/kinematics/src/profile/motion_profile.c")
PRODUCER = Producer(area="kinematics", needs=("host",), run=("--cc", "{cc}", "--out", "{out}"),
                    outputs=("profile-move.png",))
NAMES = ("10 units: rests, eases up, cruises, eases down", "2 units: too short to reach the peak")
SPEED_HEADROOM = 1.15


def move_figure(data, output):
    fig, (place, speed) = subplots(2, 1, (PANEL_SIZE * 2, PANEL_SIZE * 1.6), sharex=True)
    for index, profile in enumerate(data["profiles"]):
        samples = np.array(profile["samples"])
        colour = SLOTS[index % len(SLOTS)]
        place.plot(samples[:, 0], samples[:, 1], color=colour, label=NAMES[index])
        speed.plot(samples[:, 0], samples[:, 2], color=colour)
        speed.axhline(profile["cruise"], color=FAINT, linestyle="--", linewidth=1)
    place.set(ylabel="Place (units)")
    place.legend(fontsize=LABEL_SIZE, frameon=False)
    peak = max(profile["cruise"] for profile in data["profiles"])
    speed.set(xlabel="Time (s)", ylabel="Speed (units/s)", ylim=(0, peak * SPEED_HEADROOM))
    save(fig, output)


def main():
    data, out = probe_into_folder(__doc__, SOURCES)
    move_figure(data, out / "profile-move.png")
    for profile in data["profiles"]:
        print(f"distance={profile['distance']:.4g}: cruise={profile['cruise']:.4g}/s, "
              f"duration={profile['duration']:.4g} s, end={profile['samples'][-1][1]:.4g}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
