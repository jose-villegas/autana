"""Fly Render Lab's camera on the board from this machine's keyboard and mouse.

    python launcher/main/apps/render_lab/tools/fly.py [--scene KEY] [--board ID]

A small window: W/S forward and back, A/D left and right, E/Q up and down,
hold the right mouse button and drag to look, the wheel scales the speed,
P prints where the camera is. Closing the window puts the camera back where
the flight began. The board runs a development image with Render Lab open;
the window holds the board's lock while it is open.

The window sends what is held, not where to go: one "render fly" line a
SEND_MS with the keys down and the drag and wheel since the last line, and
the board flies the camera every frame from them (render_lab_fly.h).
"""
import argparse
import pathlib
import subprocess
import sys
import threading

ROOT = pathlib.Path(__file__).resolve().parents[5]
sys.path.insert(0, str(ROOT / "launcher/tools"))
from build.c_header import read_define  # noqa: E402

DEVICE = ROOT / "scripts/device/device.py"
FLY_HEADER = ROOT / "launcher/main/apps/render_lab/render_lab_fly.h"
AXES = int(read_define(FLY_HEADER, "RENDER_LAB_FLY_AXES"))
IDLE_MS = int(read_define(FLY_HEADER, "RENDER_LAB_FLY_IDLE_MS"))

# One line this often: the board takes one app line a frame from a short
# queue, so a faster stream would back up; the board steps every frame from
# the held keys, so the flight stays smooth at this rate. Well inside IDLE_MS,
# after which the board ends a flight it hears nothing for.
SEND_MS = 66
# A key's release waits this long for a press that cancels it: held keys
# repeat as release-press pairs on some desktops.
RELEASE_SETTLE_MS = 30
# Tk's wheel delta for one notch on Windows; X11 sends Button-4/5 instead.
WHEEL_DELTA_PER_NOTCH = 120

# Each key's axis in fly_motion_steer_t.move: (index, sign); 0 right, 1 up, 2 forward.
KEY_AXES = {
    "d": (0, 1), "a": (0, -1),
    "e": (1, 1), "q": (1, -1),
    "w": (2, 1), "s": (2, -1),
}


def move_axes(held):
    """The three key axes, each -1, 0 or 1, for the keys in `held`."""
    move = [0] * AXES
    for key in held:
        if key in KEY_AXES:
            index, sign = KEY_AXES[key]
            move[index] += sign
    return [max(-1, min(1, value)) for value in move]


def steer_line(held, drag_x, drag_y, wheel):
    """The "render fly" line for the keys held and the whole pixels and notches since the last."""
    return "render fly " + " ".join(str(value) for value in move_axes(held) + [drag_x, drag_y, wheel])


class Gathered:
    """Drag and wheel since the last line, whole units sent, the rest kept."""

    def __init__(self):
        self.drag_x = self.drag_y = self.wheel = 0.0

    def take(self):
        whole = int(self.drag_x), int(self.drag_y), int(self.wheel)
        self.drag_x -= whole[0]
        self.drag_y -= whole[1]
        self.wheel -= whole[2]
        return whole


def open_stream(board):
    command = [sys.executable, str(DEVICE)] + (["--board", board] if board else []) + ["stream"]
    return subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)


def run_window(child, scene):
    import tkinter

    root = tkinter.Tk()
    root.title("Render Lab fly")
    status = tkinter.StringVar(value="WASD move, E/Q up/down, right-drag look, wheel speed, P pose")
    tkinter.Label(root, textvariable=status, width=72, height=6, justify="left", anchor="nw").pack(fill="both")
    held = set()
    gathered = Gathered()
    pending_release = {}
    drag_from = [None]

    def send(line):
        child.stdin.write(line + "\n")
        child.stdin.flush()

    def press(event):
        key = event.keysym.lower()
        if key in pending_release:
            root.after_cancel(pending_release.pop(key))
        held.add(key)
        if key == "p":
            send("render fly pose")

    def release(event):
        key = event.keysym.lower()

        def settle():
            pending_release.pop(key, None)
            held.discard(key)

        pending_release[key] = root.after(RELEASE_SETTLE_MS, settle)

    def drag_start(event):
        drag_from[0] = (event.x, event.y)

    def drag(event):
        if drag_from[0] is not None:
            gathered.drag_x += event.x - drag_from[0][0]
            gathered.drag_y += event.y - drag_from[0][1]
            drag_from[0] = (event.x, event.y)

    def drag_end(unused_event):
        drag_from[0] = None

    def wheel(notches):
        gathered.wheel += notches

    def lost_focus(unused_event):
        held.clear()

    def tick():
        if child.poll() is not None:
            status.set("the board stream ended; see the terminal")
            return
        send(steer_line(held, *gathered.take()))
        root.after(SEND_MS, tick)

    def read_board():
        for line in child.stdout:
            if line.startswith("RENDER"):
                print(line, end="", flush=True)
                root.after(0, status.set, line.strip())

    def close():
        if child.poll() is None:
            send("render fly release")
        root.destroy()

    root.bind("<KeyPress>", press)
    root.bind("<KeyRelease>", release)
    root.bind("<ButtonPress-3>", drag_start)
    root.bind("<B3-Motion>", drag)
    root.bind("<ButtonRelease-3>", drag_end)
    root.bind("<MouseWheel>", lambda event: wheel(event.delta / WHEEL_DELTA_PER_NOTCH))
    root.bind("<Button-4>", lambda unused: wheel(1))
    root.bind("<Button-5>", lambda unused: wheel(-1))
    root.bind("<FocusOut>", lost_focus)
    root.protocol("WM_DELETE_WINDOW", close)
    threading.Thread(target=read_board, daemon=True).start()
    if scene:
        send("render scene " + scene)
    tick()
    root.mainloop()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--scene", help="switch Render Lab to this scene key first (`autana render scenes` lists them)")
    parser.add_argument("--board", "-b", help="the board's USB serial or short id (default: the only one plugged in)")
    args = parser.parse_args(argv)
    child = open_stream(args.board)
    try:
        run_window(child, args.scene)
    finally:
        if child.stdin:
            child.stdin.close()
        return_code = child.wait()
    return return_code


if __name__ == "__main__":
    raise SystemExit(main())
