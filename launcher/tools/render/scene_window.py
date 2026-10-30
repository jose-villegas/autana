#!/usr/bin/env python3
"""Play a scene's camera path in a window, every frame drawn live by the host renderer.

    scene_window.py HOST --scene ID [--scale N] [-- HOST_ARGUMENTS ...]

HOST is a scene viewer host (scene_viewer.sh --build-only prints it), run
with --serve; AUTANA_ASSET_DIR names its pack, as for any run of it. The
window shows the frame at the slider's time, drawn when asked: nothing is
recorded. Space plays and pauses, at the wall clock's pace while the host
keeps up; Left and Right step one frame time, a second with Shift; Home and
End go to the ends. The view menu lists the host's debug views, and a view
with tunables gets a box for each, applied as it changes.
"""

import argparse
import os
import pathlib
import subprocess
import sys
import time

from PIL import Image

# The time one Left or Right press moves, as the viewer's own frame time.
STEP_MS = 16
# A Shift-held press moves this much.
JUMP_MS = 1000
# How soon the window asks for the next frame while playing.
PLAY_POLL_MS = 1


class ServeError(RuntimeError):
    pass


class ViewerHost:
    """A scene viewer host under --serve: one command per line, a reply per command."""

    def __init__(self, binary, scene, arguments=(), env=None):
        self.process = subprocess.Popen([str(binary), "--scene", scene, "--serve", *arguments],
                                        stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=env)

    def ask(self, command):
        """The reply to `command`, past its "ok"; ServeError with the host's own words otherwise."""
        reply = self._send(command)
        if reply == "ok" or reply.startswith("ok "):
            return reply[3:]
        raise ServeError(reply or f"the host ended at {command!r}")

    def frame(self):
        """The current frame as a PIL image, and the microseconds its draw took."""
        header = self._send("frame").split()
        if len(header) != 5 or header[0] != "frame":
            raise ServeError(" ".join(header) or "the host ended before a frame")
        width, height, stride, draw_us = map(int, header[1:])
        data = self.process.stdout.read(stride * height)
        if len(data) != stride * height:
            raise ServeError("the host ended inside a frame")
        return Image.frombuffer("RGB", (width, height), data, "raw", "BGR", stride, -1), draw_us

    def duration_ms(self):
        return int(self.ask("duration"))

    def views(self):
        """Each view's name and its tunables' owner, None when it has none."""
        return [tuple(word.split(":", 1)) if ":" in word else (word, None) for word in self.ask("views").split()]

    def tunables(self, owner):
        """Each of `owner`'s tunables as (name, value, low, high)."""
        found = []
        for word in self.ask(f"tunables {owner}.").split():
            name, numbers = word.split("=", 1)
            found.append((name, *map(int, numbers.split(":"))))
        return found

    def close(self):
        if self.process.poll() is None:
            self._send("quit", reply=False)
            self.process.wait()

    def _send(self, command, reply=True):
        """The reply line, "" once the host has ended (it said why on stderr)."""
        try:
            self.process.stdin.write(command.encode() + b"\n")
            self.process.stdin.flush()
        except OSError:
            return ""
        return self.process.stdout.readline().decode().strip() if reply else ""


class Window:
    def __init__(self, host, scale, step_ms=STEP_MS):
        import tkinter
        from tkinter import ttk

        self.tk, self.host, self.scale, self.step_ms = tkinter, host, scale, step_ms
        self.duration = host.duration_ms()
        self.views = dict(host.views())
        self.at_ms, self.frames, self.playing, self.clock = 0, 0, False, None
        if sys.platform == "win32":
            # Screen pixels, so Windows' display scaling does not blur the frame's own.
            import ctypes

            ctypes.windll.shcore.SetProcessDpiAwareness(1)
        self.root = tkinter.Tk()
        self.root.title("scene window")
        self.picture = tkinter.Label(self.root)
        self.picture.pack()
        controls = tkinter.Frame(self.root)
        controls.pack(fill="x")
        self.play_button = ttk.Button(controls, text="Play", width=6, command=self.toggle)
        self.play_button.pack(side="left")
        ttk.Button(controls, text="<", width=3, command=lambda: self.step(-1)).pack(side="left")
        ttk.Button(controls, text=">", width=3, command=lambda: self.step(1)).pack(side="left")
        self.slider = ttk.Scale(controls, from_=0, to=self.duration, command=self.slid)
        self.slider.pack(side="left", fill="x", expand=True, padx=6)
        self.readout = tkinter.Label(self.root, anchor="w", font=("Consolas", 10))
        self.readout.pack(fill="x")
        options = tkinter.Frame(self.root)
        options.pack(fill="x")
        tkinter.Label(options, text="view").pack(side="left")
        self.view = tkinter.StringVar(value="shaded")
        ttk.OptionMenu(options, self.view, "shaded", *self.views, command=self.chose_view).pack(side="left")
        self.tunable_boxes = tkinter.Frame(options)
        self.tunable_boxes.pack(side="left", padx=6)
        self.root.bind("<space>", lambda _event: self.toggle())
        self.root.bind("<Left>", lambda _event: self.step(-1))
        self.root.bind("<Right>", lambda _event: self.step(1))
        self.root.bind("<Shift-Left>", lambda _event: self.seek(self.at_ms - JUMP_MS))
        self.root.bind("<Shift-Right>", lambda _event: self.seek(self.at_ms + JUMP_MS))
        self.root.bind("<Home>", lambda _event: self.seek(0))
        self.root.bind("<End>", lambda _event: self.seek(self.duration))
        self.root.bind("<Escape>", lambda _event: self.root.destroy())

    def use_view(self, name):
        """Attaches `name` and redraws; also what the view menu does."""
        self.view.set(name)
        self.chose_view(name)

    def chose_view(self, name):
        self.host.ask(f"view {name}")
        for child in self.tunable_boxes.winfo_children():
            child.destroy()
        owner = self.views.get(name)
        for tunable, value, low, high in self.host.tunables(owner) if owner else ():
            self.tk.Label(self.tunable_boxes, text=tunable.split(".", 1)[1]).pack(side="left")
            box = self.tk.Spinbox(self.tunable_boxes, from_=low, to=high, width=6)
            box.delete(0, "end")
            box.insert(0, str(value))
            box.configure(command=lambda tunable=tunable, box=box: self.set_tunable(tunable, box.get()))
            box.bind("<Return>", lambda _event, tunable=tunable, box=box: self.set_tunable(tunable, box.get()))
            box.pack(side="left")
        self.draw()

    def set_tunable(self, name, value):
        try:
            self.host.ask(f"set {name}={value}")
        except ServeError as error:
            self.readout.configure(text=str(error))
            return
        self.draw()

    def seek(self, at_ms):
        self.at_ms = min(max(int(at_ms), 0), self.duration)
        self.slider.set(self.at_ms)
        self.draw()

    def step(self, frames):
        self.seek(self.at_ms + frames * self.step_ms)

    def slid(self, value):
        if int(float(value)) != self.at_ms:
            self.at_ms = int(float(value))
            self.draw()

    def toggle(self):
        self.playing = not self.playing
        self.play_button.configure(text="Pause" if self.playing else "Play")
        self.clock = time.monotonic()
        if self.playing:
            self.root.after(PLAY_POLL_MS, self.play)

    def play(self):
        if not self.playing:
            return
        now = time.monotonic()
        self.at_ms = (self.at_ms + int((now - self.clock) * 1000)) % (self.duration + 1)
        self.clock = now
        self.slider.set(self.at_ms)
        self.draw()
        self.root.after(PLAY_POLL_MS, self.play)

    def draw(self):
        """The frame at the slider's time, drawn now."""
        from PIL import ImageTk

        self.host.ask(f"at {self.at_ms}")
        image, draw_us = self.host.frame()
        self.frames += 1
        if self.scale != 1:
            image = image.resize((image.width * self.scale, image.height * self.scale), Image.NEAREST)
        self.photo = ImageTk.PhotoImage(image)
        self.picture.configure(image=self.photo)
        self.readout.configure(text=f"{self.at_ms / 1000:8.3f} s / {self.duration / 1000:.3f} s   "
                                    f"frame {self.at_ms // self.step_ms}   draw {draw_us / 1000:5.1f} ms   "
                                    f"view {self.view.get()}")

    def run(self, view):
        self.use_view(view)
        self.root.mainloop()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("host", type=pathlib.Path)
    parser.add_argument("--scene", required=True)
    parser.add_argument("--scale", type=int, default=2, help="screen pixels per rendered pixel (default 2)")
    parser.add_argument("--view", default="shaded")
    argv = sys.argv[1:] if argv is None else list(argv)
    split = argv.index("--") if "--" in argv else len(argv)
    args = parser.parse_args(argv[:split])
    args.arguments = argv[split + 1:]
    if args.scale < 1:
        parser.error("--scale must be at least 1")
    host = ViewerHost(args.host, args.scene, args.arguments, env=os.environ)
    try:
        window = Window(host, args.scale)
        if args.view not in window.views:
            parser.error(f"--view is one of {', '.join(window.views)}")
        window.run(args.view)
    except ServeError as error:
        print(f"scene_window: {error}", file=sys.stderr)
        return 1
    finally:
        host.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
