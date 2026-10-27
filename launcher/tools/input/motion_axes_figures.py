"""Draw the device's chip and screen axes, and animate the motion sample over a scripted turn; --cc CC --out DIR."""
import numpy as np
from matplotlib.lines import Line2D

from input_figures import probe_data
from device_model import (MARGIN_SHARE, SCENE_ZOOM, VIEW_AZIMUTH, VIEW_ELEVATION, DeviceAnimation, DeviceModel,
                          legend_handles)
from docs.plot_style import INK, LABEL_SIZE, MUTED, SLOTS, figure, save, style_axes

AXES_SIZE = (9, 4.6)
# The front three-quarter view, and the same turned half way round to show the IMU on the back.
VIEWS = (("Front", VIEW_AZIMUTH), ("Back", VIEW_AZIMUTH + 180))
ACCELERATION = INK
ROTATION = SLOTS[4]
VECTOR_WIDTH = 2.5
# Not the axis sets' colours, so a curve is not read as an arrow.
LABEL_COLOURS = (SLOTS[1], SLOTS[3], SLOTS[6])
# The input_motion_t rotation the probe reports must match the scripted body rate to this, in deg/s.
RATE_TOLERANCE_DPS = 1.5
ROOM = 1.1
# The signed plot's top, past the peak, keeps its legend off the curves.
LEGEND_ROOM = 1.7


def axes_figure(model, pose, output):
    fig = figure(AXES_SIZE)
    extent = model.radius + MARGIN_SHARE * model.height
    for column, (name, azimuth) in enumerate(VIEWS, start=1):
        scene = fig.add_subplot(1, len(VIEWS), column, projection="3d", computed_zorder=False)
        scene.set_axis_off()
        for setter in (scene.set_xlim, scene.set_ylim, scene.set_zlim):
            setter(-extent, extent)
        scene.set_box_aspect((1, 1, 1), zoom=SCENE_ZOOM)
        scene.view_init(elev=VIEW_ELEVATION, azim=azimuth)
        model.draw(scene, pose)
        scene.set_title(name, fontsize=LABEL_SIZE, color=MUTED)
    fig.legend(handles=legend_handles(), loc="outside lower center", ncol=2, fontsize=LABEL_SIZE, frameon=False)
    save(fig, output)


def rotation_figure(data, model, output):
    frames = data["frames"]
    times = np.array([frame["t"] for frame in frames])
    rotation = np.array([frame["rotation"] for frame in frames])
    # The axis the code's rotation stands for, by the right-hand rule: the device vectors behind its labels.
    labels = -model.screen_axes
    axes = rotation @ labels
    worst = np.abs(axes - np.array([frame["rate"] for frame in frames])).max()
    if worst > RATE_TOLERANCE_DPS:
        raise SystemExit(f"motion_axes_figures: rotation is {worst:.1f} deg/s from the scripted body rate")
    turn_speed = np.array([frame["turn_speed"] for frame in frames])
    peak = turn_speed.max()

    animation = DeviceAnimation(model, plots=2)
    signed, speed = animation.plots
    for column, name, colour in zip(rotation.T, "xyz", LABEL_COLOURS):
        signed.plot(times, column, color=colour, label=f"rotation.{name}")
    signed.axhline(0, color=MUTED, linewidth=0.7)
    signed.set(xlim=(0, times[-1]), ylim=(-peak * ROOM, peak * LEGEND_ROOM))
    signed.set_ylabel("deg/s", fontsize=LABEL_SIZE)
    signed.legend(fontsize=LABEL_SIZE, loc="upper center", frameon=False, ncol=3, handlelength=1.2)
    style_axes(signed, "rotation")
    speed.plot(times, turn_speed, color=ROTATION)
    speed.set(xlim=(0, times[-1]), ylim=(0, peak * ROOM))
    speed.set_xlabel("Time (s)", fontsize=LABEL_SIZE)
    speed.set_ylabel("deg/s", fontsize=LABEL_SIZE)
    style_axes(speed, "turn_speed")
    animation.legend([Line2D([], [], color=ACCELERATION, label="acceleration (1 g)"),
                      Line2D([], [], color=ROTATION, label="rotation axis, right-hand rule")]
                     + legend_handles()[1:])

    def draw(index):
        frame = frames[index]
        pose = np.array(frame["pose"])
        down = pose @ model.to_device(frame["acceleration"]) * animation.unit
        spin = pose @ axes[index] / peak * animation.unit
        x, y, z = frame["rotation"]
        animation.frame(pose, [(animation.beside, down, ACCELERATION, VECTOR_WIDTH),
                               (animation.left, spin, ROTATION, VECTOR_WIDTH)], frame["t"],
                        f"t = {frame['t']:4.1f} s    rotation ({x:+4.0f}, {y:+4.0f}, {z:+4.0f}) deg/s    "
                        f"turn_speed {frame['turn_speed']:3d} deg/s", chip_axes=False)

    animation.save(output, len(frames), draw)


def main():
    data, output, cc = probe_data(__file__, None, __doc__)
    model = DeviceModel(cc)
    axes_figure(model, np.array(data["frames"][0]["pose"]), output / "motion-axes.png")
    rotation_figure(data, model, output / "motion-rotation.gif")


if __name__ == "__main__":
    main()
